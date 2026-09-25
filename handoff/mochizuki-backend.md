# Handoff: backend mochizuki (terceiro runtime AMD NR)

Registro completo do trabalho de 24/09/2026 que colocou a rede neural do projeto
**mochizuki0323/DLSSNR-AMD** (Vulkan) para rodar dentro deste fork do OptiScaler, ao lado dos runtimes
danielblnc e lmxxf. Escrito para quem continuar, pessoa ou IA: o que foi feito e em que ordem, o que foi
medido, onde estão os arquivos, como reproduzir e o que falta.

Caminhos: `<REPO>` é este checkout. A seção 7 lista o estado local da máquina de desenvolvimento, com
caminhos reais.

**Estado:** funciona no Cyberpunk 2077 (testado pelo usuário). Instalado para teste no GTA V Enhanced,
aguardando teste. **Nada commitado ainda.**

---

## 1. Resumo

- O DLSSNR-AMD reimplementa a rede NR do DLSS 5 (NVIDIA `nvngx_dlssnr.dll` 310.8.0) em compute shaders
  Vulkan que usam FP8 (`VK_KHR_cooperative_matrix` + `VK_EXT_shader_float8`, instruções WMMA da RDNA4).
  Os pesos vêm do DLL da NVIDIA do próprio usuário.
- O host do autor só funciona com o jogo rodando sobre vkd3d-proton: ele pede o `VkDevice` e a command
  buffer do jogo às interfaces de interop do vkd3d. Em D3D12 nativo isso não existe.
- **Solução usada aqui:** o jogo continua em D3D12 nativo. A rede roda num **device Vulkan próprio** na
  mesma GPU, encaixada entre as duas metades da command list do jogo, com o mesmo mecanismo de corte de
  lista que o fork já usava para o lmxxf (HIP).
- O runtime é `MochizukiNrRuntime.dll`, que implementa a ABI do lmxxf (`LmxxfNrApi.h`). O OptiScaler
  escolhe com `[DlssNr] NrBackend=mochizuki`.
- Esta versão só liga e desliga (`Enable NR`), roda antes do Super Resolution e usa o histórico temporal
  do próprio modelo com os vetores de movimento do jogo. Sem controles.

| Número principal | Valor |
|---|---|
| GPU da rede em 1920x1080 (RX 9070 XT, driver 26.8.1) | 9,9 ms (lmxxf: 18,5 ms) |
| Primeira construção da rede num jogo | ~55 s (compilação de pipelines, uma vez por executável) |
| Construções seguintes | 1,5 a 3 s |
| Estabilidade em movimento com histórico | 73% mais estável que sem histórico |

---

## 2. Passo a passo (24/09/2026, em ordem)

1. **Análise do repositório.** Upstream criado no mesmo dia: v0.0.1, commit `791b046`, MIT. Conclusão:
   o host dele não serve para D3D12 nativo; o caminho é device Vulkan próprio com interop D3D12,
   reaproveitando o encaixe do lmxxf. O usuário decidiu implementar direto.
2. **Extração do modelo.** `dlssnr.bin` gerado do `nvngx_dlssnr.dll` 310.8.0 local (SHA-256
   `e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e`): 599 entradas, 147.756.560 bytes,
   todas conferidas contra os hashes do autor. O script bash do autor falha no Windows; os passos Python
   foram rodados à mão (seção 6.2).
3. **Shaders.** glslang 16.5.0 para Windows (a versão que o autor fixa). `build_network.py` gerou 39
   pipelines da rede, 4 temporais e 6 de runtime. Duas compilações independentes deram SPIR-V idêntico.
4. **Upstream no repositório.** Núcleo, shaders e scripts copiados para `third_party/mochizuki`
   (56 arquivos, 1,8 MB) com `UPSTREAM.md` registrando o commit e o patch.
5. **Runtime.** `OptiScaler/dlssnr/backend/mochizuki_runtime/MochizukiNrRuntime.cpp`: ABI do lmxxf,
   device Vulkan escolhido pelo LUID da GPU do jogo, buffers D3D12 compartilhados, fence D3D12 importada
   como timeline semaphore, construção da rede em thread separada.
6. **Build com MSVC.** `tools/build-mochizuki-runtime.cmd`. Precisou de um patch de uma linha no código do
   autor, de um shim para `dirent.h` e dos headers do Vulkan SDK 1.4.357.
7. **Primeiro teste fora do jogo (`mz_bench`).** Construção em 24 s a frio e 1,5 s com cache. Rede em
   9,9 a 10,2 ms em 1080p; frame completo em 12 a 13 ms. Saída conferida: alinhada com a entrada
   (correlação 0,984 sem deslocamento), sem NaN.
8. **Integração no OptiScaler.** `Kind.h`, `Selector`, `AmdBridge`, `LmxxfBackend` (agora recebe o nome
   da DLL), menu (combo com os runtimes instalados e uma seção do mochizuki só com status e custo) e
   `Config.h`. OptiScaler compilado; clang-format limpo.
9. **Proteção contra os hooks Vulkan do OptiScaler.** A criação do device do runtime roda dentro de
   `ScopedSkipVulkanHooks` e `ScopedCreatingD3DDevice`.
10. **Teste de fases (`mz_phases`).** Numa só sessão: 1080p, 720p em R11G11B10 com vetores R32G32 em
    1440p, 1440p, 1080p. Construção em segundo plano, drenagem e reconstrução funcionaram.
11. **Instalação no Cyberpunk** com `install-mochizuki.ps1` (backup do `dxgi.dll` anterior; INI do
    pacote 0.3.0 com `NrBackend=mochizuki`).
12. **Documento de arquitetura** para outra IA portar (seção 7).
13. **Teste do usuário no Cyberpunk: funcionou.** Sessão de ~20 min sem erro; rede em 1920x1080 (pronta
    em 54,8 s na primeira vez) e em 1280x720 (2,7 s). O log mostrou `motion 0x0 DXGI 0`: os vetores de
    movimento não chegaram e a rede rodou sem histórico.
14. **Correção dos vetores.** O Cyberpunk manda os vetores em `R16G16B16A16_FLOAT` e o runtime só
    aceitava 2 canais. Agora aceita 4 canais (o núcleo lê só R e G) e registra formato recusado no log.
15. **Validação do sinal e da escala dos vetores (`mz_pan`).** Conteúdo andando 2 px por frame, vetores
    em RGBA16F. Vetores corretos: 73% mais estável que sem histórico. Sinal invertido: pior que sem
    histórico. A convenção está certa (seção 4.6).
16. **Cyberpunk atualizado com o runtime corrigido e GTA V Enhanced instalado.**
17. **Harnesses guardados** em `<REPO>/exports/mochizuki-harness/` e registro publicado (artefato e este
    arquivo).

---

## 3. Resultados

### 3.1 Medições

| Medida | Valor | Onde |
|---|---|---|
| GPU da rede, 1920x1080 | 9,9 ms | `mz_bench`, 100 frames |
| Frame completo, 1920x1080 (cópias, submit, espera) | 12,0 ms | `mz_bench`, mediana |
| GPU da rede, 2560x1440 | ~23 ms | `mz_phases` |
| Construção a frio, harness | 24 s | primeira vez por executável |
| Construção a frio, Cyberpunk 1080p | 54,8 s | log do jogo |
| Construção com cache | 1,5 s | harness |
| Reconstrução no jogo ao trocar resolução | 2,7 s | Cyberpunk, 720p |
| Memória da rede em 1080p | ~537 MB | 384 MB de ativações, 153 MB de pesos |
| Referência: lmxxf (HIP), 1080p | 18,5 ms | medido na 0.3.0 |
| Referência: autor, 1080p | 9,4 ms (Windows), 6,2 ms (Linux) | README do upstream |

### 3.2 Estabilidade em movimento (`mz_pan`)

Quanto o efeito da rede (saída menos entrada) muda de um frame para o outro, acompanhando o conteúdo que
anda 2 px por frame. Menor é mais estável.

| Caso | Mudança entre frames | Efeito médio |
|---|---|---|
| Sem vetores (como no primeiro teste do Cyberpunk) | 0,00541 | 0,0206 |
| Vetores corretos (mv = -2 px) | 0,00148 | 0,0288 |
| Vetores com sinal invertido | 0,00576 | 0,0303 |
| Imagem parada (piso) | 0,00078 | 0,0299 |

### 3.3 Robustez

| Teste | Resultado |
|---|---|
| Saída alinhada com a entrada (correlação por deslocamento de -2 a +2 px) | pico em 0, 0,984 |
| Troca de resolução no meio da sessão | drena, reconstrói e segue |
| Cor R16G16B16A16 e R11G11B10 | funciona, sem NaN |
| Vetores R16G16, R32G32 (1440p com cor em 720p) e R16G16B16A16 | funciona |
| Cyberpunk 2077, ~20 min, 1080p e 720p | funcionou, sem erro no log |

---

## 4. Arquitetura (essencial)

Detalhes do runtime também em `OptiScaler/dlssnr/backend/README.md` (seção mochizuki) e em
`third_party/mochizuki/UPSTREAM.md`.

### 4.1 Componentes

```
Jogo (D3D12 nativo)
 └─ OptiScaler (dxgi.dll)
     ├─ AmdBridge              intercepta o Evaluate do upscaler e troca a cor pelo resultado do NR
     ├─ Kind / Selector        NrBackend = daniel | lmxxf | mochizuki
     ├─ LmxxfBackend           host da ABI "lmxxf"; carrega a DLL pelo nome
     └─ submission/            proxy de command list: corta a lista e chama um callback "between"

MochizukiNrRuntime.dll (MSVC, C ABI de LmxxfNrApi.h)
 ├─ MochizukiNrRuntime.cpp     sessão, interop D3D12 <-> Vulkan, jobs
 └─ nr_runtime.cpp (+ nr_graph.cpp), nr_native_plan.cpp   núcleo do upstream
      └─ vulkan-1.dll -> driver Vulkan da AMD

Ao lado da DLL:
 dlssnr-amd/dlssnr.bin         pesos (gerados do DLL da NVIDIA)
 dlssnr-amd/shaders/           SPIR-V da rede, runtime/ e temporal/
 dlssnr-amd/pipeline.cache     gerado na primeira execução
 mochizuki_nr.log              log do runtime e do núcleo
```

### 4.2 Fluxo de um frame

```
Thread de render (Evaluate)                     Thread de submit (ExecuteCommandLists)
PrepareFrame  : rede pronta? recursos da geometria
RecordInputs  : cor e vetores -> buffers compartilhados (segmento 1)
SplitSegments : corta a lista
RecordOutputs : buffer de saída -> textura "result" (segmento 2)
AmdBridge troca a cor do upscaler por "result"
                                                Execute(segmento 1)
                                                between -> EnqueueHip:
                                                   fila D3D12: Signal(fence, N)
                                                   Vulkan: espera N, buffer -> imagens,
                                                           record_engine, imagem -> buffer,
                                                           sinaliza N+1
                                                   fila D3D12: Wait(fence, N+1)
                                                Execute(segmento 2)  (cópia + upscaler)
                                                Submitted -> Retire
```

A fila usada é a que executa a lista. A sincronização é toda pela fence compartilhada, então qualquer
fila do mesmo device serve. Entre frames, a cadeia sinal/espera ordena o reuso dos buffers e imagens.
Três slots (command buffer + `VkFence`) protegem só o reuso dos command buffers no CPU.

### 4.3 Device Vulkan próprio

- Instância Vulkan 1.3, sem camadas. Physical device escolhido pelo **LUID** do adaptador D3D12
  (`VkPhysicalDeviceIDProperties::deviceLUID`). Família `GRAPHICS | COMPUTE`, uma fila.
- Features: `cooperativeMatrix`; `shaderFloat8` e `shaderFloat8CooperativeMatrix`;
  `storageBuffer16BitAccess`; `storageBuffer8BitAccess`, `shaderFloat16`, `shaderInt8`,
  `vulkanMemoryModel`, `timelineSemaphore`; `subgroupSizeControl`, `synchronization2`;
  `workgroupMemoryExplicitLayout` com 8 e 16 bits.
- Extensões: `VK_KHR_cooperative_matrix`, `VK_EXT_shader_float8`, `VK_KHR_workgroup_memory_explicit_layout`,
  `VK_KHR_external_memory_win32`, `VK_KHR_external_semaphore_win32`.
- O construtor do `nr::Runtime` **checa** as features em vez de habilitá-las e faz submit na fila para
  subir os pesos; por isso a fila tem um mutex compartilhado entre frames e construção.

### 4.4 Interop

- **Buffers, não texturas:** importar textura exige que as duas APIs concordem no tiling. Buffers são
  lineares. Custa uma cópia a mais de cada lado, desprezível em 1080p.
- Buffer: `CreateCommittedResource` com `D3D12_HEAP_FLAG_SHARED` (tamanho arredondado para 64 KB),
  `CreateSharedHandle`; no Vulkan, `VkExternalMemoryBufferCreateInfo` com
  `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT`, tipo de memória por
  `vkGetMemoryWin32HandlePropertiesKHR`, alocação **dedicada** com `VkImportMemoryWin32HandleInfoKHR`.
  O handle NT continua com quem o criou; fecha na destruição.
- Fence: `CreateFence(D3D12_FENCE_FLAG_SHARED)` + `CreateSharedHandle`; no Vulkan, semaphore timeline e
  `vkImportSemaphoreWin32HandleKHR` com `VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT`. O contador é
  o mesmo dos dois lados.
- Três buffers por geometria: entrada (cor), saída (resultado) e vetores.

### 4.5 Formatos e footprints

| DXGI (inclui TYPELESS) | VkFormat | Bytes |
|---|---|---|
| `R16G16B16A16_FLOAT` | `R16G16B16A16_SFLOAT` | 8 |
| `R32G32B32A32_FLOAT` | `R32G32B32A32_SFLOAT` | 16 |
| `R11G11B10_FLOAT` | `B10G11R11_UFLOAT_PACK32` | 4 |
| `R10G10B10A2_UNORM` | `A2B10G10R10_UNORM_PACK32` | 4 |
| `R8G8B8A8_UNORM(_SRGB)` | `R8G8B8A8_UNORM` | 4 |
| `B8G8R8A8_UNORM(_SRGB)` | `B8G8R8A8_UNORM` | 4 |
| vetores `R16G16_FLOAT` / `R32G32_FLOAT` | `R16G16_SFLOAT` / `R32G32_SFLOAT` | 4 / 8 |
| vetores `R16G16B16A16_FLOAT` / `R32G32B32A32_FLOAT` | os mesmos em SFLOAT | 8 / 16 |

- Cor em float (RGBA16F, R11G11B10, RGBA32F) é tratada como luz linear: `linear_input = true`,
  `white_point = 1.0`.
- Footprint do retângulo `w x h`: `pitch = align(w * bytes, 256)`; no D3D12 o footprint usa o formato
  da textura; no Vulkan `bufferRowLength = pitch / bytes`.
- A textura `result` tem o desc da cor do jogo e fica sempre em `NON_PIXEL_SHADER_RESOURCE`, o mesmo
  contrato da saída do lmxxf.

### 4.6 Vetores de movimento e histórico

- Convenção do OptiScaler/NGX: `valor * MV_Scale = pixels da extensão dos vetores`; posição anterior =
  atual + mv.
- O shader do autor amostra o histórico em `(uv + mv) * extent`, com `mv` em UV normalizado. Logo
  `motion_scale = MV_Scale / extensão dos vetores` (saída quando o jogo não usa `MVLowRes`, renderização
  quando usa). Validado com `mz_pan` (seção 3.2).
- `reset` do frame = reset do jogo, ou frame não contínuo (`frame_id` fora de sequência ou mais de 250 ms
  desde o anterior), ou `ResetHistory`/`CancelUnsubmitted` pendente.
- Depth ainda não é passado.

### 4.7 Construção da rede

- Por extensão e formato de cor, numa `std::thread`. Enquanto constrói, `PrepareFrame` devolve
  `LMXXF_NR_UNAVAILABLE` com "building the network" e o frame passa sem NR.
- Troca de extensão ou formato: drena as duas filas, destrói a rede e reconstrói.
- Cache de pipelines em `dlssnr-amd/pipeline.cache`. O driver AMD separa as entradas por executável:
  a primeira vez em cada jogo compila tudo.
- Build que falha marca a sessão como falha; o motivo fica no log.

### 4.8 Mudanças no OptiScaler

| Arquivo | Mudança |
|---|---|
| `dlssnr/backend/Kind.h` | `Kind::Mochizuki`, `ParseKind("mochizuki")` |
| `dlssnr/backend/Selector.cpp/.h` | `SubmissionHooksWanted()` vale também para mochizuki |
| `dlssnr/amd/AmdBridge.cpp` | `HasFiles()` e criação de `LmxxfBackend(..., L"MochizukiNrRuntime.dll")` |
| `dlssnr/backend/LmxxfBackend.cpp/.h` | nome da DLL como parâmetro; configuração do lmxxf só quando `Lmxxf()`; `PrepareSession` dentro de `ScopedSkipVulkanHooks` + `ScopedCreatingD3DDevice`; `UNAVAILABLE` do mochizuki só atualiza o status; prefixo de status `mochizuki` |
| `dlssnr/DlssNr_Menu.cpp` | combo com os runtimes instalados; seção do mochizuki com status e custo |
| `Config.h` | comentário de `NrBackend` |
| `dlssnr/backend/README.md` | seção mochizuki |
| `.gitignore` | `third_party/mochizuki/toolchain/` (glslang baixado) |

---

## 5. Onde está cada coisa (no repositório)

| Caminho | O que é |
|---|---|
| `OptiScaler/dlssnr/backend/mochizuki_runtime/MochizukiNrRuntime.cpp` | o runtime (novo) |
| `OptiScaler/dlssnr/backend/mochizuki_runtime/compat/dirent.h` | shim POSIX para MSVC (novo) |
| `OptiScaler/dlssnr/backend/lmxxf_runtime/LmxxfNrApi.h` | a ABI C compartilhada |
| `third_party/mochizuki/` | upstream fixado em `791b046` + `UPSTREAM.md` (novo) |
| `tools/build-mochizuki-runtime.cmd` | build da DLL e dos shaders (novo) |
| `exports/mochizuki-runtime/` | saída do build, fora do git, com `dlssnr-amd/` |
| `exports/mochizuki-harness/` | `mz_bench.cpp`, `mz_phases.cpp`, `mz_pan.cpp`, `build-harness.cmd`, fora do git |
| `exports/cyberpunk-test/` | `install-mochizuki.ps1`, `remove-test.ps1` e as listas instaladas, fora do git |

---

## 6. Como reproduzir

### 6.1 Runtime e shaders

Num prompt "x64 Native Tools" do Visual Studio, com o Vulkan SDK 1.4.357 ou mais novo (`VULKAN_SDK`) e
Python no PATH:

```
cd <REPO>
tools\build-mochizuki-runtime.cmd exports\mochizuki-runtime
```

O script baixa o glslang 16.5.0 na primeira vez (hash conferido) e compila a DLL com as constantes da
rede (as mesmas de `third_party/mochizuki/windows/build/arch/rdna4.sh`).

### 6.2 Modelo

Use `python`, não `python3` (no Windows, `python3` pode ser o atalho da Microsoft Store). `W` é uma pasta
de trabalho.

```
cd <REPO>\third_party\mochizuki\linux\package\model-tools
python inspect_nr.py <nvngx_dlssnr.dll 310.8.0> --output W\inventory --extract > W\inspect.json
copy descriptor.json W\graph\
python unpack_swin_family.py --artifacts W --output W\unpacked --max-c 256
python unpack_splitswin.py --artifacts W --output W\unpacked-splitswin
python unpack_vit.py --artifacts W --output W\unpacked-vit
python unpack_preblock.py W\inventory\weights\block0.layer0.layer.bin W\unpacked-preblock
python unpack_postblock.py W\inventory\weights\block70.layer0.layer.bin W\unpacked-postblock
python pack_model.py --root W --list model-files.txt --out dlssnr.bin --verify model-files.sha256
```

Resultado esperado: `599 entries, 140.9 MiB`. Coloque em `exports\mochizuki-runtime\dlssnr-amd\`.

### 6.3 Testes fora do jogo

```
cd <REPO>\exports\mochizuki-harness
build-harness.cmd
mz_bench.exe ..\mochizuki-runtime\MochizukiNrRuntime.dll 100
mz_phases.exe ..\mochizuki-runtime\MochizukiNrRuntime.dll 1920x1080:r16:40,1280x720:r11:40:2560x1440,2560x1440:r16:30
mz_pan.exe ..\mochizuki-runtime\MochizukiNrRuntime.dll 60 n,p,w,s
```

- `mz_bench`: custo, saída diferente da entrada, sem NaN.
- `mz_phases`: formatos e trocas de resolução numa sessão.
- `mz_pan`: `n` sem vetores, `p` vetores corretos, `w` sinal invertido, `s` imagem parada. `p` tem que
  ser bem menor que `n` e `w`.

A saída de `printf` da DLL (CRT estático) se mistura com a do harness; os resultados confiáveis estão nas
linhas do próprio harness e no `mochizuki_nr.log` ao lado da DLL.

### 6.4 OptiScaler

```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" OptiScaler.sln -p:Configuration=Release -p:Platform=x64 -m
```

### 6.5 Instalar e remover num jogo (teste)

```powershell
$s = '<REPO>\exports\cyberpunk-test'
& "$s\install-mochizuki.ps1" -Game '<GAME_DIR>' -List "$s\installed-<jogo>.txt"
& "$s\remove-test.ps1"      -Game '<GAME_DIR>' -List "$s\installed-<jogo>.txt"
```

A instalação copia o OptiScaler novo como `dxgi.dll` (com backup do anterior), a pasta `OptiScaler\`
de dependências do pacote 0.3.0, `MochizukiNrRuntime.dll`, `dlssnr-amd\` e um `OptiScaler.ini` do pacote
com `NrBackend=mochizuki` (só se não houver INI). No jogo: DLSS nas opções gráficas, Insert, `Enable NR`.

---

## 7. Estado local (máquina de desenvolvimento)

| Caminho | O que é |
|---|---|
| `E:\Projetos\decompile_nr\nvngx_dlssnr.dll` | origem do modelo (310.8.0) |
| `E:\Games\Cyberpunk2077\bin\x64\` | instalação de teste; backup em `_optiscaler_backup_2026-09-24_1931build`; runtime corrigido; lista em `installed.txt` |
| `E:\Games\GTAVEnhanced\` | instalação de teste (pasta estava limpa); lista em `installed-gtav.txt` |
| `C:\Users\matheus\Desktop\mochizuki-runtime-arquitetura.md` | arquitetura detalhada com código e guia de porte para outros hosts |
| https://claude.ai/artifact/2kYTAUa8gJY3hKcZmjVwSD | este mesmo registro como página (privada) |

---

## 8. Problemas encontrados e soluções

| Problema | Causa | Solução |
|---|---|---|
| Host do autor não serve em D3D12 nativo | depende do `VkDevice` do vkd3d-proton | device Vulkan próprio com interop D3D12 |
| Extração do modelo falha no Windows | script bash usa `/tmp`; `python3` é o atalho da Store | passos Python à mão com `python` |
| Erro C2397 no MSVC | conversão de enum para flags entre chaves em `nr_runtime.cpp` | cast `VkImageAspectFlags(...)`, registrado no `UPSTREAM.md` |
| `dirent.h` ausente | header POSIX em `telemetry.hpp` | shim em `compat/` |
| Sem `VK_EXT_shader_float8` | headers Vulkan 1.4.310 do OptiScaler | Vulkan SDK 1.4.357 |
| `Get-FileHash` indisponível | PowerShell chamado de dentro do `cmd` | download e hash do glslang em Python |
| Device do runtime interceptado | hooks do OptiScaler no `vulkan-1.dll` alteram extensões e registram o device | `ScopedSkipVulkanHooks` + `ScopedCreatingD3DDevice` no `PrepareSession` |
| Construção lenta de novo em outro programa | cache de pipelines da AMD separado por executável | esperado, uma vez por jogo |
| Cyberpunk sem histórico temporal | vetores em `R16G16B16A16_FLOAT` recusados | formatos de 4 canais aceitos; recusas no log |
| `small` quebra a compilação do harness | macro do `windows.h` | outro nome |

---

## 9. Roadmap

### Fase 0: versão mínima (concluída)

- Runtime Vulkan com a ABI do lmxxf, liga e desliga pelo `Enable NR`, só antes do SR.
- Histórico temporal com os vetores do jogo, validado com `mz_pan`.
- Funcionando no Cyberpunk 2077.

### Fase 1: validação em jogos (em andamento)

- **GTA V Enhanced:** instalado, aguardando teste (modo história, BattlEye desligado, DLSS nas opções).
- Cyberpunk de novo com o histórico: conferir no log `motion 1920x1080 DXGI 10` e a imagem em movimento.
- Mais jogos D3D12 com DLSS, incluindo um que mande vetores na resolução de saída.
- Sessões longas: TDR, VRAM, alt-tab, menus que trocam a resolução, geração de frames ligada.

### Fase 2: controles (próxima)

- Detail e colour strength: `transfer_strength`/`color_strength` para `Controls.detail_strength` (0 a 2)
  e `colour_strength` (0 a 4), mais `max_ratio`.
- Controles do modelo: style, local tone, local structure, skin e automatic mask (os mesmos do
  danielblnc; o núcleo já aceita).
- `Controls.intensity` e `set_history_strength`.
- Passes: construir com `max_passes` maior e usar `Controls.passes` (cada pass custa o tempo da rede e
  uma imagem de histórico).
- `model_scale`: rede abaixo da resolução do frame; custo cai com o quadrado da escala.
- White point a partir do pré-exposure do jogo.
- Seção do menu com esses controles.

### Fase 3: qualidade e custo (depois)

- Depth em `EngineFrame.depth` (melhora bordas em movimento).
- Ler a cor sem supor o estado do recurso.
- Menos cópias: conversão em compute no D3D12 ou importação direta de texturas.
- Suporte depois do SR, para jogos com Ray Reconstruction.

### Fase 4: lançamento (depois)

- Commit (quando o usuário pedir).
- `tools/PACKAGE_RELEASE.ps1` levando `MochizukiNrRuntime.dll` e `dlssnr-amd\shaders\`, sem o modelo;
  comentário do INI com `mochizuki`.
- Instalador: instalar o runtime, os shaders e o modelo como os outros runtimes, conferindo
  cada arquivo por hash.
- Instalador: componente no `payload.json`, teste de versão e publicação.
- Notas de release, `handoff/README.md` e este arquivo atualizados.

### Fase 5: manutenção (contínua)

- Acompanhar o upstream (nasceu em 24/09 e deve mudar rápido); script de sincronização como o do lmxxf.
- A cada atualização: recompilar e rodar `mz_bench`, `mz_phases` e `mz_pan` antes de instalar.

---

## 10. Decisões pendentes e riscos

**Decisões do usuário:**
- Em qual versão do instalador o mochizuki entra.
- Se `NrBackend=auto` deve escolher o mochizuki quando ele estiver instalado.
- Quando commitar e em qual versão lançar.

**Riscos:**
- GTA V Enhanced traz BattlEye: só modo história, com o BattlEye desligado. Nunca em jogo online.
- Primeira execução de cada jogo: ~55 s sem NR enquanto os pipelines compilam.
- ~0,55 GB de VRAM a mais em 1080p, num contexto Vulkan separado.
- A cópia de entrada supõe o estado informado da cor; se o jogo estiver em outro, o debug layer do D3D12
  reclama.
- Upstream em 0.0.1, com a árvore Windows descrita pelo autor como experimental.
- Os sliders de força e passes do menu não chegam a este runtime.
