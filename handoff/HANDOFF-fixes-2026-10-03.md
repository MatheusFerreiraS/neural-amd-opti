# Handoff — rodada de correções de 2026-10-03

Tudo aqui está **sem commit e sem release**: o usuário quer revisar antes ("Por enquanto sem release, quero ver
tudo antes"). Nada foi publicado no HF nem no GitHub nesta rodada. Os três repositórios têm mudanças locais:

| Repositório | Branch @ base | O que mudou |
|---|---|---|
| `opti-fixes\` (worktree do `opti`) | `fixes` @ `283fdb0a` (0.4.9-amd-nr) | async, proteção sem travar a sessão, log/marcador do mochizuki, correção dos shaders do mochizuki para o driver 32.0.32015 |
| `reshade\` (add-on) | `master` @ `551eea0` (v0.7.9) | async no painel 64-bit, proteção sem travar a sessão, protocolo da ponte v6, volta automática do mochizuki para o danielblnc |
| `AMD-NR-ReShade-Installer\` | `master` @ `bbe817b` (v0.7.10) | versão do runtime do Daniel por jogo, política RX 9000, selos, async por padrão, mensagens de arquivo preso, aviso de `d3d11.dll` de outro mod |

O `opti-fixes` é uma pasta separada só para estas correções; o NR/FG sem upscaler continua no `opti` principal,
intocado. `external\` do `opti-fixes` é junction para o checkout principal. Build: `tools\build-optiscaler-fast.cmd`.

---

## 1. Pedidos do usuário e como ficaram

### 1.1 Escolher a versão do runtime do Daniel (a partir da 0.4.3)
- **Instalador.** `payload.json` ganhou `releases.runtime` com 0.6.0, 0.5.1, 0.5.0 e 0.4.3; cada uma carrega o
  `runtime` (pass1 patcheado + pesos, para o add-on) e o `opti-runtime` (`dlssnr_amd_runtime-<v>.dll`, cru, para o
  OptiScaler). Os arquivos já estão no HF (`runtime/<v>/` e `opti-runtime/`).
- `PayloadManifest.WithRuntime(version, async)`, `RuntimeVersions()`, `RuntimeChoice` (sobrevive a `With()` de
  uma versão do add-on/OptiScaler aplicada depois), `PayloadPins.RuntimeChosen` / `RuntimeAsync`.
- `Work.RuntimeRunsOn` (tabela `AddonAcceptedRuntimes` + `AcceptedRuntime` do OptiScaler): só aparecem as versões
  que o add-on/OptiScaler escolhido roda.
- UI: linha "Versão do danielblnc" (`DanielVersionRow`) na ficha do jogo; escolha salva em `GameStore.DanielRuntime`.
- **Política por placa** (pedido seguinte do usuário), em `Work.Runtimes.cs`:
  - RX 9000: todas as versões; **0.4.3 é o padrão** com selo verde **Recomendado**; as outras com selo laranja
    **Instável** (`Work.RuntimeOffer/RuntimePick/BadgeFor`, `RuntimeOption.cs`, estilos `Border.tag.good/.warn`).
  - RX 6000/7000: só a mais nova (0.6.0), sem selo.
  - Textos em 21 idiomas (`Str.RuntimeRecommended`, `Str.RuntimeUnstable`, `Str.DanielVersionTip`).
- Testes: `RuntimeChoiceTests.cs`.

### 1.2 Botão para ligar/desligar o modo assíncrono
- **Add-on:** "Timing" (Mesmo quadro / Assíncrono) agora também no painel 64-bit (`ui/sections/performance.cpp`,
  `NetworkTiming`), grava `Inline` no `amd-nr.ini`. A runtime trava o modo quando monta o staging, então vale na
  próxima abertura (`RuntimeInline()` em `runtimes.inc` lê uma vez por sessão). Em async: `RuntimeBusy()` não usa
  `kJobCounter` (ele não anda em async) e a checagem de `kInlineActive` só vale em mesmo-quadro.
- **OptiScaler:** `[DlssNr] AmdAsync` (Config.h/.cpp, `OptiScaler.ini`), combo "Timing" no menu
  (`DlssNr_Menu.cpp`), `AmdBridge::AsyncSession()` (lido uma vez). Em async: força a posição "depois do quadro
  pronto" (uma correção atrasada um quadro se acumularia no histórico do upscaler), um pass só, e os slots se
  aposentam só pelo fence (`AmdPreSr.cpp`, `async`; recusas logadas por amostragem).
- **Instalador:** em RX 9000 com versão instável, a instalação liga o async (`Work.GoesInAsync`: `Inline=0` no
  `amd-nr.ini` / `AmdAsync=true` no `OptiScaler.ini`, só essa chave). 0.4.3 e RX 6000/7000 não mexem.
  Observação: o OptiScaler 0.4.9 publicado ignora `AmdAsync`; só o build do `opti-fixes` lê.
- Medido fora do jogo (runtime real, RX 9070 XT, 1080p): fila do jogo ~0,45 ms/quadro em async contra 15–20 ms em
  mesmo-quadro; quadros que chegam com a rede ocupada são pulados (saem sem NR).
- `framecheck` conta quadros pulados em async; `amd_runtime_init_smoke` ganhou `AMD_TEST_ASYNC`.

### 1.3 Proteção: desligar sem travar a sessão
- `StallWatch::Clear()` (add-on `core/shared/stall_watch.h` e opti `StallWatch.h`).
- Add-on: `JobGate` desliga `Enabled`, guarda `standDownMs`, limpa o watch; o painel mostra "desligado: a GPU segurou
  um job por X s; marque Ligado para tentar de novo". Ponte 32-bit: `WireStatus.stallMs`, **protocolo v6**
  (`bridge_ipc.h`, `frontend32.cpp`, `host64.cpp`, `panel32.cpp`, `protocol_test.cpp`, `docs/x86bridge.md`).
- OptiScaler: `AmdBridge` desliga `DlssNrEnabled` (volátil), guarda a nota (`StandDownNote()`), toast + texto em
  vermelho no menu; religar o "Enable NR" tenta de novo.
- **Não testado em jogo.** O framecheck não passa pelo `JobGate`. Para testar: `StallStandDownMs=5` /
  `AmdStallStandDownMs=5`, abrir, ver desligar, religar.

### 1.4 RX 9000: mochizuki padrão no add-on, lmxxf no OptiScaler
- Sem checkbox experimental: em RX 9000 o mochizuki entra sempre (`WantsMochizuki`); add-on usa mochizuki quando
  não há `NrBackend` (`runtime_choice.h`); OptiScaler.ini novo/nosso recebe `NrBackend=lmxxf` (`RunsLmxxf`).
- **Atenção:** ver a seção 3 — no driver 32.0.32015 o mochizuki publicado (0.4.9) derruba o jogo. Não publicar a
  política "mochizuki padrão em RX 9000" antes de publicar os shaders corrigidos.

---

## 2. Outras mudanças desta rodada

### Instalador
- **Arquivo preso nomeia o processo** (`Engine.Holders.cs`, Restart Manager): "dxgi.dll is open in BatmanAK.exe
  (PID …)…"; usado na instalação, pré-check e desinstalação (`Engine.OpenElsewhere`, `Engine.Write`).
- **Aviso de `d3d11.dll` de outro mod** nas rotas D3D10/11 (`Work.CheckForeignD3d11`). Origem: report do Batman
  (RX 6600, wrapper d3d11 de 2017 + `ori_d3d11.dll`; ReShade "Skipping swap chain… without a proxy Direct3D
  device"; o crash não tinha relação com o Daniel — add-on/runtime nunca carregaram).
- Testes: `HeldFilesTests.cs`; 4 testes antigos ajustados para o texto novo. Último `dotnet test`: 340/340;
  `gate.ps1 -Ui`: build, line-limit, ui e flows OK (o teste rodou depois dos ajustes).
- **Build de revisão:** `AMD-NR-ReShade-Installer\publish-review-fixes\AMD-NR-ReShade-Installer.exe`. O
  `config.json` dessa pasta aponta `ManifestUrl` para `http://127.0.0.1:9/payload.json` (falha rápido e cai no
  `payload.json` local, que tem `releases.runtime`). `dotnet publish` sobrescreve esse config: refazer depois.

### Add-on: volta automática quando o mochizuki morre no build
- A DLL do mochizuki mantém `dlssnr-amd\build.pending` durante o build. Na abertura, se ele existe e o mochizuki
  está escolhido, o add-on roda o danielblnc, grava `NrBackend=danielblnc`, renomeia para `build.failed` e mostra
  aviso no painel (`runtime_choice::MochizukiBuildDied`, `mochizuki.inc`, `general.cpp`). Escolher o mochizuki de
  novo tenta outra vez (com log detalhado). Testado com o framecheck no driver que quebra: ciclo completo OK.
- Ponte 32-bit: a volta acontece no helper; o aviso só aparece no log.
- Falso alarme possível: fechar o jogo durante os segundos de compilação conta como build morto.

### OptiScaler: input no Assetto Corsa (patch do player GhostNappa, revisado)
Origem: `E:\FDM-downloads\Optiscaler fix - Assetto Corsa.txt` (partes B a E; a parte A não veio). Pendência antiga:
`opti\handoff\assetto-corsa-menu-input.md`.
- **B, como veio:** dispositivos DirectInput "Other" (volante, pedal, joystick) não são mais bloqueados com o menu
  aberto (`ShouldBlockDirectInputOtherLocked` = false). Zerar o `DIJOYSTATE` punha os eixos no extremo (pedal no
  fundo).
- **C, como veio:** atalho aceito marca o `lastInputTick` (`menu_common.cpp`), então a mesma tecla vista por mensagem e
  por polling não alterna o menu duas vezes.
- **D+E, com ajuste:** o hook `WH_MOUSE_LL` também entra no processo do jogo, mas só com o menu aberto, a WndProc
  tomada e um mouse DirectInput em modo exclusivo (`hkDirectInputSetCooperativeLevel`, vtable 13,
  `DirectInputMouseExclusive`). Os botões do hook vão para `LowLevelMouseDown` e o polling soma (OR) com o estado
  assíncrono, em vez de ser desligado; a posição continua do polling. O patch original ligava o hook o tempo todo e
  em qualquer jogo sem WndProc (risco de travar o mouse do sistema e de perder os cliques do MSFS).
- Build: `exports\assetto-input\OptiScaler.dll` (= `exports\release-local`). Não testado no Assetto (não instalado
  aqui); falta o retorno do GhostNappa.

### DLL do mochizuki (opti-fixes, `OptiScaler/dlssnr/backend/mochizuki_runtime/`)
- `BuildMarker` (`build.pending` / `build.failed`) e modo detalhado: com um build anterior morto, o próximo loga
  cada pipeline do prewarm e do núcleo, com o `.spv` (`mz_interpose`: `Capture::Verbose`, `NameOf`).

---

## 3. mochizuki × driver AMD 32.0.32015.2008 — investigação e correção

### Sintoma
Relatos (BeamNG, RX 9060 XT) e o próprio Cyberpunk do usuário: `mochizuki_nr.log` termina em "building the
network" e o jogo cai. Reproduzido na RX 9070 XT do usuário assim que ele instalou o 32.0.32015: 0xC0000005 em
`vkCreateComputePipelines`, DLL nova e antiga. O mesmo build funcionava no 32.0.31041.

### Causa 1 — crash na compilação
- Ferramenta `spvprobe` (compila um `.spv` com o layout real, um por processo): só 4 de 60 shaders derrubam o driver:
  `g_attn`, `g_ffwd3`, `g_ffwd3w`, `g_fswinpds128` (os `temporal_*` eram falso positivo de layout errado na sonda).
- Gatilho (recorte de fonte do `ffwd3`): loops que indexam arrays de `coopmat` com a variável do loop
  (`h[k]`; no `fswinpds128`, loops `for (int m = mh; …)` que o `unroll_glsl.py` deixava porque `mh` é constante de
  uma cópia externa).

### Causa 2 — imagem errada (manchas, mais escura, flicker no Cyberpunk)
- Com o crash contornado, a rede rodava mas com a imagem errada. Descartados com teste na GPU: layout dos fragmentos
  FP8 A/B/acumulador (FP32 e FP16), MMA FP8 16×16×16 exata contra a CPU, conversões FP8 (as com clamp saturam; a
  conversão crua acima de ~464 vira NaN, mas forçar saturação em tudo não resolve), subnormais FP16, barreiras
  (`NR_COHERENT_BARRIERS=0`, `NR_INV_BARRIERS=0`), `NR_NO_PERSIST`, `spirv_precise.py` do upstream, a DLL das
  referências. Interop/encode corretos (saída = entrada com força 0).
- **Localização camada a camada:** `nr_graph` (o front end de linha de comando do próprio `nr_graph.cpp`,
  compilado fora da DLL) com o plano nativo salvo (`planout`), `--no-reuse --dump-all`. Gravado no 31041
  (gabarito, determinístico) e no 32015: tudo idêntico até a etapa 059; **primeira divergência na 060,
  `b031l3 CCVit1DAttention` (kernel `vitattn`)**. Dentro dele (`NR_VDUMP`): denominador do softmax certo,
  numerador saturado em ±448.
- **Causa:** o mesmo defeito do driver, aqui gerando conta errada em silêncio — loops `uint` sobre arrays de
  `coopmat` (`ctx[b][n]`, `vf[n]`…). O `unroll_glsl.py` só desenrolava loops `int`.

### Correção (opti-fixes, `third_party/mochizuki/windows/build/`)
- `unroll_glsl.py`: `with_value()` (substitui a constante da cópia externa nos cabeçalhos dos loops internos e
  desenrola de novo) e suporte a loops `uint` (`16u`, `uint(...)`, cópias `const uint V = Nu`).
- `build_network.py`: `UNROLLED = {fswin_t, ffwd3_t, attn, vit_attn}`.
- Mudam 6–8 shaders; os outros ficam idênticos. Conferido: as transformações não mudam o resultado (no 31041 os
  shaders corrigidos dão saída **bit a bit idêntica** à referência; no 32015, formas diferentes do mesmo kernel dão
  saídas idênticas).
- Build com tudo: `opti-fixes\exports\mochizuki-fix2\` (DLL + `dlssnr-amd\shaders`). **Esse é o conjunto a
  publicar** quando o usuário aprovar (nova versão do componente `mochizuki` no payload).
- `nr_runtime.cpp` ganhou `NR_GRAPH_ARGS` (diagnóstico, só por variável de ambiente). Decidir se fica.

### Resultado no 32015 (contra o 31041)
| caso | erro médio antes | depois |
|---|---|---|
| 1080p (1/2/3 passes, R10, F32, prep, sem movimento) | ~0,08 | ~0,001 |
| 1707×961, 4K | 0,09–0,17 | 0,001–0,002 |
| **1280×720** | 0,10 | **0,011, ~2% mais escura** |
~0,001 = 1/3 de degrau de 8 bits (mesma ordem da diferença mochizuki × DLL da NVIDIA em `docs/ngx-verification`
do upstream); vem de arredondamento de um degrau FP8 em 0,07% dos valores da etapa 060, propagado.
Tempo de rede igual ao de antes (~8–9 ms a 1080p no Cyberpunk).

### 720p / FSR Qualidade, Balanceado, Ultra Desempenho (achado depois, `mochizuki-fix3`)
- Vídeo do usuário (1080p de saída): manchas como as da 1080p antes da correção em Qualidade (1280×720),
  Balanceado (~1129×635) e Ultra Desempenho (640×360), não em Desempenho (960×540). O NR roda na resolução de
  render.
- Padrão: a atenção do ViT trabalha em blocos de 64 tokens. Resoluções boas têm tokens múltiplos de 64
  (540p 192, 961p 448, 1080p 640); as ruins não (360p 96, 635p 240, 720p 288) e passam pelo corpo "guardado" do
  último bloco (`include/vit_attn_vt_chunk.glsl`, `NR_VT_GUARD 1`).
- Causa: o 32015 calcula errado a escolha entre um fragmento `coopmat` carregado e um zerado
  (`if (...) load else kfr = NR_FRAG_A(0.0)`, `if (...) pe = NR_FRAG_E4M3(0.0)`).
  Prova sem o driver antigo: `nr_graph --vattn-tokens 608` a 1080p (tokens 576+ são zero, então a saída esperada
  é a média harmônica das saídas de 576 e 640). Corpo original: 15% abaixo do previsto (corr 0,977); corpo
  corrigido: razão 1,0009 (corr 0,9994).
- Correção (shader): K da tile de padding carregado na tile viva clampada (`min(j0 + kb, jlast)`), logit da
  padding = 0 e probabilidade = 0 nos valores f16, sem select de matriz.
- `unroll_glsl.py`: também desenrola loops com `continue` (cópia em `do { } while (false)`) e loops até
  `.length()` de `coopmat` 16×16 de subgrupo (8 por lane, subgrupo fixo em 32). Sobram só loops de contagem
  dinâmica. Mudam `attn`, `vitattn*`, `fswin*32`, `fswinimage*`.
- Bateria (`wt\mz-driver-32015\harness\cmp.py mkfix5`) contra o 31041: 1080p/961p/4K ~0,001 (igual ao fix2);
  720p 0,011 → **0,0040** (corr 0,9996, +0,7% de brilho). Ainda ~4× as outras: sobra um desvio pequeno em 720p.
- Export: `opti-fixes\exports\mochizuki-fix3\` (mesma DLL do fix2 + shaders novos). **Substitui o fix2 como
  conjunto a publicar.** Instalado no Cyberpunk (backup `_optiscaler_backup_2026-10-03_premzfix3`).
- Testes desta parte: `wt\mz-driver-32015\nrgraph\vitmask.py`, `vittail.py`, `gemmtail.py` (os GEMMs do ViT
  não têm problema de bloco final).

- Teste em jogo do fix3 (Cyberpunk, 2026-10-03 22:54): o usuário confirmou que as manchas sumiram. Log sem erros:
  shaders recompilados ao detectar a mudança, rede montada em 1080p, 720p (1 a 3 passes), 635p, 540p, 360p,
  560×350 e 587×330.

### Rodada no driver 31041 (fecha o 720p)
- `driver-round.ps1 old` e `new`: com o fix3, os dois drivers dão saída **idêntica byte a byte** em todos os casos
  (1080p, 961p, 4K, 720p, 635p, 360p, passes 2/3, formatos; só `prep` varia 0,001 entre execuções). O 32015 está
  totalmente contornado.
- O "resto de 0,004" em 720p não era defeito: a referência `b_fin_720` (shaders originais no 31041) é que está
  errada. O kernel original do mochizuki erra o bloco incompleto da atenção do ViT **em qualquer driver** quando
  há dados reais depois do fim da sequência (como a 720p).
  Prova (`nrgraph\vitreal.py`, 1080p `--vattn-tokens 544`, no 31041): a implementação independente do mesmo
  shader (`NR_VTRANS=0`, com e sem `NR_VKMASK`) bate com o fix3 (0,2–0,5% de bytes, nível de arredondamento; em
  640 tokens original e independente já diferem 0,44%); o original difere 58%, o fix2 83%.
- 1080p: fix3, fix2 e a referência diferem ~0,001 entre si (execuções determinísticas). É arredondamento FP8 das
  formas diferentes do código, mesma ordem da diferença mochizuki × DLL da NVIDIA.
- Possível aviso ao upstream (mochizuki0323): bloco final parcial do `vit_attn_vt_chunk.glsl` (NR_VT_GUARD 1)
  dá resultado errado; a correção do fix3 serve para eles também.

---

## 4. Instalações de teste nos jogos

| Jogo | O que está instalado | Backup |
|---|---|---|
| Cyberpunk 2077 (`E:\Games\Cyberpunk2077\bin\x64`) | `dxgi.dll` = OptiScaler do `opti-fixes`; `dlssnr_amd_pass1-3.dll` = runtime 0.6.0 crua; mochizuki = `mochizuki-fix3` (DLL + shaders); `OptiScaler.ini` com `NrBackend = mochizuki` (o usuário escolheu) | `_optiscaler_backup_2026-10-03_prefixes`, `…_premzverbose`, `…_premzfix`, `…_premzfix2`, `…_premzfix3` |
| GTA IV 32-bit (`E:\SteamLibrary\steamapps\common\Grand Theft Auto IV\GTAIV`) | `amd-nr.addon32` + `amd-nr-host64.exe` (protocolo v6) + runtime 0.6.0 patcheada; mochizuki = `mochizuki-fix3` (antes havia um de 2026-09-27) | `_amdnr_backup_2026-10-03_prefixes`, `_amdnr_backup_2026-10-03_premzfix3` |

O `OptiScaler.ini` do Cyberpunk ainda tem a sobra `AmdEncoding = 0`.

---

## 5. Ferramentas (cópia estável em `E:\Projetos\opti_rr_dlss5\wt\mz-driver-32015\`)

- `probe\spvprobe.exe file.spv KINDS PUSH` — compila um shader com layout por binding (`B` buffer, `I` storage
  image, `S` sampler) e diz se o driver aguenta. `bisect.py` / `cut.py` / `mini.py` — recortes de define/fonte/
  shaders mínimos. `fp8run.exe` + `*.comp` — testes na GPU (`LAYOUT`, `MMA`, `ACC`, `CMP`, `STRIDE`).
  `satfp8.py` — marca conversões FP8 como saturantes (experimento, não resolveu).
- `nrgraph\nr_graph.exe` (build: `build_nrgraph.cmd`), `planout.exe W H FILE` (plano nativo), `plan1080.txt`,
  `plan720.txt`, entradas `in1080/in720.rgba32f`, gabarito `dump_old_31041_1080p\` (index + 184 arquivos;
  `184-k566` varia entre execuções, ignorar), `out_old.rgba32f`. `vitloop.py` / `vitedit.py` — variantes do
  `vitattn` comparadas na etapa 060. `unroll_uint.py` / `unroll_full.py` — protótipos do desenrolador.
- `harness\` — `mz_e2e` (D3D12 → DLL do mochizuki; `E2E_PAN/E2E_W/E2E_H/E2E_FORMAT/E2E_PREP/E2E_NOMOTION/
  E2E_STRENGTH`), `run.cmd`, `battery.ps1` (10 casos), referências `b_fin_*.bin` (driver 31041, 2026-10-01/02) e
  a pasta de teste `mkfix2`.

---

## 6. Decisões em aberto para o usuário

1. Publicar o mochizuki corrigido (`mochizuki-fix3`) e só então a política "mochizuki padrão em RX 9000".
   Enquanto não, ou manter o checkbox antigo, ou o instalador não deixar o mochizuki padrão no driver 32015+.
2. Avisar o upstream (mochizuki0323) do bloco final parcial da atenção do ViT (seção 3).
3. Testar em jogo: proteção sem travar a sessão (seção 1.3) e async em placa fraca (RX 9060/6000).
4. `NR_GRAPH_ARGS` no `nr_runtime.cpp`: manter como diagnóstico ou tirar.
5. Ordem de publicação quando aprovado: opti (tag nova), add-on, instalador, payload (`releases.runtime`,
   componente `mochizuki` novo), espelho no Extras.
