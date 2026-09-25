# Handoff: backend mochizuki (terceiro runtime AMD NR)

Registro completo do trabalho de 24/09/2026 que colocou a rede neural do projeto
**mochizuki0323/DLSSNR-AMD** (Vulkan) para rodar dentro deste fork do OptiScaler, ao lado dos runtimes
danielblnc e lmxxf. Escrito para quem continuar, pessoa ou IA: o que foi feito e em que ordem, o que foi
medido, onde estão os arquivos, como reproduzir e o que falta.

Caminhos: `<REPO>` é este checkout. A seção 7 lista o estado local da máquina de desenvolvimento, com
caminhos reais.

**Estado:** funciona no Cyberpunk 2077 (testado pelo usuário). Instalado para teste no GTA V Enhanced,
aguardando teste. **Nada commitado ainda.**

**Atualização de 25/09/2026:** duas ondas de trabalho (24-25/09) acrescentaram controles, correções de
estabilidade, desempenho e ferramentas de teste. Resumo, números e pendências na **seção 11**. O candidato à
primeira versão é o snapshot `<REPO>/exports/mochizuki-test-wave2/`, ainda não testado em jogo.

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
  do próprio modelo com os vetores de movimento do jogo. Sem controles; a Fase 2 (§9) os acrescenta.

| Número principal | 24/09 (0.3.0) | 25/09 (onda 2, §11.4) |
|---|---|---|
| GPU da rede em 1920x1080 (RX 9070 XT, driver 26.8.1) | 9,9 ms (lmxxf: 18,5 ms) | 8,9 ms |
| Primeira construção da rede | ~55 s no jogo, 24,8 s no harness | 5,8-6,0 s no harness com o manifesto de prewarm; no jogo, não medido |
| Construções seguintes | 1,5 a 3 s | 1,5 a 1,7 s no harness |
| Estabilidade em movimento com histórico | 73% mais estável que sem histórico | igual (saída bit a bit idêntica) |

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

Números de 24/09, antes das ondas 1 e 2. Os atuais estão em §11.4 (o "~23 ms" em 1440p veio de média móvel e
construção a frio; medido depois: 16,7-17,3 ms na 0.3.0).

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

Desde a onda 2 (S3) são **duas** fences compartilhadas, uma por direção: `toVk` (a fila do jogo sinaliza
`produced`, o Vulkan espera) e `fromVk` (o Vulkan sinaliza `finished`, a fila do jogo espera), de modo que
nenhuma anda para trás. Um frame vindo de outra fila do jogo espera antes o último frame da fila anterior; o
runtime declara `ANY_QUEUE`, então o host não recria a sessão ao trocar de fila. Um watchdog libera as filas do
jogo se o lado Vulkan parar (§11.3).

### 4.3 Device Vulkan próprio

- Instância Vulkan 1.3, sem camadas. Physical device escolhido pelo **LUID** do adaptador D3D12
  (`VkPhysicalDeviceIDProperties::deviceLUID`). Família `GRAPHICS | COMPUTE`; desde a onda 2 (S4), duas filas
  quando a família as tem (prioridades 1,0 e 0,5): os frames usam a primeira e a construção da rede a segunda.
- Features: `cooperativeMatrix`; `shaderFloat8` e `shaderFloat8CooperativeMatrix`;
  `storageBuffer16BitAccess`; `storageBuffer8BitAccess`, `shaderFloat16`, `shaderInt8`,
  `vulkanMemoryModel`, `timelineSemaphore`; `subgroupSizeControl`, `synchronization2`;
  `workgroupMemoryExplicitLayout` com 8 e 16 bits.
- Extensões: `VK_KHR_cooperative_matrix`, `VK_EXT_shader_float8`, `VK_KHR_workgroup_memory_explicit_layout`,
  `VK_KHR_external_memory_win32`, `VK_KHR_external_semaphore_win32`.
- O construtor do `nr::Runtime` **checa** as features em vez de habilitá-las e faz submit na fila para
  subir os pesos; por isso, com uma fila só, ela tem um mutex compartilhado entre frames e construção. Com
  duas, a construção não disputa a fila dos frames.

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
  o mesmo dos dois lados. Desde a onda 2 são duas fences assim, uma por direção (§4.2).
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
- Troca de extensão ou formato (texto de 24/09; atualizado na onda 2, S4/S5): a thread de render não espera
  mais a GPU (com uma fila de jogo só; com várias, os buffers antigos ainda são drenados). A rede antiga é
  liberada por uma thread própria depois que o Vulkan termina o último frame dela, e a nova é construída na
  segunda fila. Mudança só de passes, escala do modelo ou entrada linear: os frames
  continuam na rede antiga até a nova ficar pronta ("keep serving"). Com `MochizukiDynamicResolution` em
  `auto` (padrão) ou `always`, mudar o subrect de render dentro do balde não reconstrói nada (§9, Fase 2).
- Cache de pipelines em `dlssnr-amd/pipeline.cache`. O driver AMD separa as entradas por executável:
  a primeira vez em cada jogo compila tudo. Desde a onda 2 (P3), `dlssnr-amd/prewarm/manifest.txt` descreve
  os 32 pipelines e o runtime os compila em paralelo (8 threads, prioridade abaixo do normal) antes do núcleo,
  e o cache é gravado de novo depois dos adaptadores e dos temporais.
- Build que falha (texto de 24/09): marcava a sessão como falha. Desde a onda 2 (S2) só a perda do device é
  definitiva; falta de memória tenta de novo após 5, 30 e 120 s, os outros erros quando a chave muda.

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

Mudanças das ondas 1 e 2 no OptiScaler (saída, backoff, chaves, menu, cancelamento de job, barreiras dos
upscalers, resolução dinâmica): §11.2 e §11.3.

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
| `OptiScaler/dlssnr/backend/mochizuki_runtime/MochizukiNrControls.h` | exports de controle (`MochizukiNrSetControls`, `GetInfo`, `GetControlDefaults`, `GetFeatures`), onda 1 |
| `OptiScaler/dlssnr/backend/mochizuki_runtime/mz_interpose.cpp/.h` | captura dos pipelines, manifesto e prewarm paralelo, onda 2 (P3) |
| `tools/sweep_mochizuki_knobs.py` | varredura dos knobs de `pipelines.json` numa cópia do upstream, onda 2 (P6) |
| `exports/mochizuki-work/` | plano, goldens, ferramentas, relatórios (`wave1-report.md`, `wave2-report.md`) e saídas de cada pacote, fora do git |
| `exports/mochizuki-test-wave2/` | snapshot congelado para o teste em jogo (candidato à primeira versão), fora do git |

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

Desde a onda 1 há também `mz_timing`, `mz_stress`, `mz_compare.py` e os scripts de equivalência e desempenho
em `exports/mochizuki-work/tools/`; os procedimentos exatos estão em `exports/mochizuki-work/PROCEDURES.txt` e
o resumo em §11.5.

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

Atenção (25/09): `install-mochizuki.ps1` copia o runtime de `exports\mochizuki-runtime\`, que ainda é o build
anterior às ondas 1 e 2, e não copia `dlssnr-amd\prewarm\`. Para testar o snapshot da onda 2, copie à mão
`exports\mochizuki-test-wave2\` (`OptiScaler.dll` como `dxgi.dll`, `MochizukiNrRuntime.dll` e `dlssnr-amd\`
inteira, com `prewarm\manifest.txt`), ou use o script de instalação de snapshot quando existir (pacote D3).

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
- **Próximo passo:** testar no Cyberpunk o snapshot `exports/mochizuki-test-wave2/` (candidato à primeira
  versão) com a lista de verificação de `exports/mochizuki-work/wave2-report.md` §7, que junta as pendências da
  onda 1 (saída durante a construção, controles, alt-tab, barreiras dos upscalers) às da onda 2 (primeira
  construção com prewarm, rede mantida durante a reconstrução, resolução dinâmica, VRAM, watchdog). Nada disso
  foi rodado em jogo pelos agentes.

### Fase 2: controles (implementada; falta o teste em jogo)

- Chaves próprias `[DlssNr] Mochizuki*` (Config.h, Config.cpp, INI do pacote, README). Os ajustes do
  mochizuki não vêm de nenhuma chave do danielblnc, do lmxxf ou do NVIDIA (`TransferStrength`,
  `ColourStrength`, `Passes`, `AmdModelScale`, `AmdDynamicScale`, `DlssNrRRWorkingScale` e as do modelo).
  Todo padrão é o da rede, então um INI sem essas chaves roda como antes.
- `AmdModelScale` e `AmdDynamicScale` não afetam o mochizuki: a espera de estabilização do
  `AmdBridge::Run` ignora os dois para ele (S5). O aviso e o botão do menu saíram.
- Resolução dinâmica: `MochizukiDynamicResolution` (`auto`, padrão; `exact`; `always`) vira o `drs_mode`
  do runtime. Em `auto`, quando o subrect de render fica menor que a textura de cor, a rede roda num balde
  (por eixo, o maior subrect visto, arredondado para 64 px, dentro da textura): mudar o subrect dentro dele
  só zera o histórico, sem reconstruir a rede nem esperar a estabilização (a espera do bridge passa a
  olhar a textura de cor). O balde cresce com um subrect maior e encolhe depois de 30 s de frames de uma
  mesma textura de cor com subrects pelo menos 128 px menores nos dois eixos. `exact` reconstrói a cada
  resolução, como antes; `always` usa o balde em todo frame, também entre realocações da textura. O padrão do
  próprio runtime (struct zerada, `MochizukiNrGetControlDefaults`) é `exact`; quem pede `auto` é o host. Um
  `drs_mode` acima de 2, ou vetores de movimento num formato que não permite blit, rodam como `exact` (o log diz
  quando os vetores impedem o balde). O comentário de `drs_mode` em `MochizukiNrControls.h` descreve os três modos.
- Por frame, em `LmxxfNrFrameInfo`: `MochizukiDetailStrength` (0 a 2), `MochizukiColourStrength` (0 a 4;
  o lmxxf continua em 0 a 2; o padrão do host é 0, a cor do próprio jogo na luminância da rede, enquanto o
  do runtime é 1), `MochizukiModelScale` (0,25 a 1, em passos de 0,05) e `MochizukiPasses`
  (1 a 3). Escala e passes reconstroem a rede, construída para exatamente esse número de passes.
- Pelo export `MochizukiNrSetControls` (`MochizukiNrControls.h`): intensity, style, local tone, local
  structure, skin, automatic mask, highlight guard (`max_ratio`, 1 a 8), `set_history_strength`,
  `set_white_point` (fixo em `MochizukiWhitePoint`, 1 por padrão; sem pré-exposure por enquanto), linear
  input, apply model e os overrides dos passes 2 e 3. O host só chama quando algo muda ou a sessão é nova.
- `MochizukiTemporal` liga o histórico; sem a chave, vale o `LmxxfTemporal`.
- Seção do menu com esses controles; passes e resolução aplicam ao soltar o slider. O status mostra a
  extensão do modelo, o tempo da rede (mediana e p95), a última construção e avisa quando os vetores de
  movimento são recusados.
- `EvaluateAtSeam` ignora `ApplyAfterRR` para lmxxf e mochizuki; o menu avisa e oferece um botão para
  desligar a chave.
- Falta no jogo: cada slider muda a imagem ao vivo; passes e resolução reconstroem uma vez, ao soltar; o
  status mostra o tempo da rede. `MochizukiModelScale` abaixo de 1 ainda não é recomendado: em panorâmica
  lenta sobra um resíduo alternado (ver C1).

### Fase 3: qualidade e custo (em parte feita nas ondas 1 e 2)

- Feito (§11): custo da rede −0,86 ms em 1080p e −1,23 ms em 1440p, saída bit a bit idêntica (P4, P6, P9);
  primeira construção 24,8 → ~6 s no harness (P3); estabilidade (S1-S5, H1, S8); resolução dinâmica (S5).
- Suspensos pelo usuário para a primeira versão: pré-filtro no downscale do núcleo para `MochizukiModelScale`
  abaixo de 1 (C3; hoje há tremulação em panorâmica lenta), prioridade de residência de memória (P5) e prewarm
  opcional no início da sessão (P8).
- Candidatos medidos e não construídos (P18): fundir `gemmproj→gemmvqkvnorm` (0,15 ms de barreira em 1080p) e
  `vitattn→gemmproj` (0,04-0,10 ms).
- Guarda do índice de 16 bits da fila persistente (S9, em andamento à parte): só importa acima de ~21,8 MP de
  entrada (8K).
- Depth em `EngineFrame.depth` (melhora bordas em movimento).
- Ler a cor sem supor o estado do recurso.
- Menos cópias: conversão em compute no D3D12 ou importação direta de texturas.
- Suporte depois do SR, para jogos com Ray Reconstruction.
- Paridade com o Linux (6,2 ms) não é alcançável sem paridade de compilador (LLPC contra ACO).

### Fase 4: lançamento

- Lançado no 0.4.0-amd-nr (25/09):
  - o zip do OptiScaler leva o suporte ao mochizuki (backend, as 29 chaves `Mochizuki*` no INI, o menu), mas não
    o runtime;
  - o instalador (AMD-NR ReShade Installer) instala o mochizuki como os outros runtimes: `MochizukiNrRuntime.dll`,
    `dlssnr-amd\shaders\`, `dlssnr-amd\prewarm\manifest.txt` e o modelo `dlssnr.bin`, conferidos por hash;
  - o manifesto de pré-aquecimento precisa ser regerado sempre que os shaders mudarem.
- Pendente: ferramenta de extração nativa no Windows (`tools/extract-mochizuki-model.*`, pacote D1), que não
  existe ainda.

### Fase 5: manutenção (contínua)

- Acompanhar o upstream (nasceu em 24/09 e deve mudar rápido); script de sincronização como o do lmxxf
  (`tools/sync-mochizuki-upstream.ps1`, pacote D2, planejado à parte). Os patches locais a reaplicar estão em
  `third_party/mochizuki/UPSTREAM.md` (entradas 1-5, §11.6).
- A cada atualização: recompilar e rodar a equivalência contra os goldens (`tools/mz_equiv.py`), `mz_stress all`
  e `mz_perf.py --baseline` (§11.5) antes de instalar; regerar o manifesto de prewarm se os shaders mudarem.

---

## 10. Decisões pendentes e riscos

**Decisões do usuário:**
- Se `NrBackend=auto` deve escolher o mochizuki quando ele estiver instalado.
- Quando commitar e em qual versão lançar.
- Dispensas de critério aceitas para a 0.4.0, registradas com números e motivo em
  `exports/mochizuki-work/release/WAIVERS.md`:
  - C1 (escala 0,5 e tremulação em 0,75): só com `MochizukiModelScale` abaixo de 1 (padrão 1).
  - P3: adaptadores em 0,08 s na segunda execução, pagos em toda construção morna também no padrão (só custo de
    início, a saída não muda); e a exceção à DECISIONS 7 sob falta de processador.
  - S5 (faixa de ±30% em 2304x1296; análise do Q1 pendente): vale no padrão `MochizukiDynamicResolution=auto`
    sempre que o subrect do jogo é menor que a textura de cor.
  - As notas de medição do M0.
  O usuário pode revogar qualquer uma antes do commit.
- Quando retomar C3, P5 e P8 (suspensos para a primeira versão).

**Riscos:**
- GTA V Enhanced traz BattlEye: só modo história, com o BattlEye desligado. Nunca em jogo online.
- Primeira execução de cada jogo: ~55 s sem NR enquanto os pipelines compilam (24/09). Com o manifesto de
  prewarm ao lado da DLL, 6 s no harness (contra 21,6 s sem ele); no jogo ainda não medido. Sem o manifesto o
  runtime compila em série e o grava ao final.
- VRAM: a rede de 1080p ocupa ~0,7 GB a mais (698 MB medidos pelo S2), num contexto Vulkan separado. Durante
  uma reconstrução que mantém a rede antiga as duas coexistem (a checagem de VRAM conta as duas). A estimativa de
  VRAM foi calibrada só nesta GPU e neste driver.
- Uma espera de slot de 250 ms estourada 3 vezes seguidas conta como perda do device e desliga o NR até o fim
  do processo; um travamento legítimo de ~1 s da GPU poderia disparar isso (não visto em teste). O watchdog
  declara o device perdido após 5 s sem progresso do lado Vulkan.
- Com `MochizukiDynamicResolution` em `auto`, quando a resolução de render cai a rede continua custando o
  tamanho do balde; a primeira vez que o subrect fica menor que a textura custa um `PrepareFrame` de ~12-13 ms.
- Resoluções de entrada acima de ~21,8 MP (8K) disparam o limite de 16 bits da fila persistente do upstream
  (quadro de ~0,5 s e imagem errada) até o S9 entrar.
- A cópia de entrada supõe o estado informado da cor; se o jogo estiver em outro, o debug layer do D3D12
  reclama.
- Upstream em 0.0.1, com a árvore Windows descrita pelo autor como experimental.
- Os controles do mochizuki usam só as chaves `Mochizuki*`: um valor ajustado para o danielblnc ou o lmxxf
  (`TransferStrength`, `Passes`, `AmdModelScale`) não vale aqui, e vice-versa. Exceção: sem
  `MochizukiTemporal`, vale o `LmxxfTemporal`. Cada mudança de passes, de escala do modelo ou do tamanho da
  rede reconstrói a rede; desde a onda 2 (S4) passes e escala mantêm os frames na rede antiga durante a
  reconstrução, e só a mudança de tamanho ainda deixa um ou dois segundos sem NR. Mais passes custam mais
  VRAM. O tamanho da rede é a resolução de render com `MochizukiDynamicResolution=exact`; com `auto`
  (padrão) ou `always` é o balde, que só muda quando chega um subrect maior, quando a textura de cor muda
  ou quando ele encolhe (ver §9).

---

## 11. Ondas 1 e 2 (24-25/09/2026)

Trabalho feito por agentes em pacotes, com plano, revisão independente de cada pacote e integração ao fim de
cada onda. Plano e decisões do usuário em `exports/mochizuki-work/plan/` (`plan.md`, `DECISIONS.md`,
`WAVE2.md`); relatórios de integração em `exports/mochizuki-work/wave1-report.md` e `wave2-report.md`.

Regras seguidas: nada commitado, nada instalado em jogo; toda mudança no runtime ou nos shaders precisa manter a
saída **bit a bit idêntica** aos goldens de 24/09 com os valores padrão; desempenho decidido por pares
intercalados na mesma sessão (5 pares), nunca pela média do status.

| Onda | Pacotes | Resultado |
|---|---|---|
| 1 (24/09 à noite a 25/09 04h) | M0 harness e goldens; S1 higiene do runtime; C1 controles no runtime; H1 ciclo de vida no host; C2 chaves, menu e docs; S8 cancelamento de job; S6 barreiras dos upscalers; S7 hooks Vulkan; P1 kit de ISA; P2 varreduras; P4 leituras em palavras | integrada; snapshot `exports/mochizuki-test-wave1/` |
| 2 (25/09) | S2 erros e recuperação; S3 sincronização entre APIs; P3 prewarm paralelo; S4 construção fora do caminho do frame; S5 resolução dinâmica; P6 knobs do LLPC; P9 limpeza de codegen; P7 opções de unroll (nada adotado); P18 barreiras (nada adotado) | integrada; snapshot `exports/mochizuki-test-wave2/`, candidato à primeira versão |
| suspensos pelo usuário | C3 estabilidade com escala do modelo abaixo de 1; P5 residência de memória; P8 prewarm opcional no início da sessão | para a primeira versão |

### 11.1 Integração final (25/09, 17h20-17h50)

| Verificação | Resultado |
|---|---|
| Build do runtime | BUILD_OK, 0 avisos; a DLL importa só `vulkan-1.dll` e `KERNEL32.dll` |
| Equivalência com os goldens | exata (`mz_bench` 1080p x3 e 1440p, `mz_phases`, `mz_pan`); também com `drs_mode=1`, o padrão do host |
| `mz_stress all` | 21 de 21 casos; com resolução dinâmica em `auto` (cópia do S5 com `drspad`), 22 de 22. Com o `mz_stress` do harness já com a resolução dinâmica (RL2): 21/21, e 22/22 em `auto` e em `always` |
| Build do OptiScaler (Release x64) | 0 erros; 32 avisos, todos anteriores |
| clang-format 20.1.8 | 0 violações nos 21 arquivos C/C++ alterados |
| Chaves e padrões | 29 chaves `Mochizuki*` conferidas; padrões do host iguais aos da rede |
| Mudanças inesperadas | nenhuma: 33 caminhos alterados desde o backup de 24/09, todos de algum pacote |

### 11.2 Controles, chaves e menu

Todas em `[DlssNr]`; o padrão de cada uma reproduz a saída de 24/09. Detalhes de cada controle em §9, Fase 2.

| Chave | Padrão | Faixa e efeito |
|---|---|---|
| `MochizukiTemporal` | true (sem a chave, vale `LmxxfTemporal`) | histórico temporal |
| `MochizukiHistoryStrength` | 1 | 0 a 1, ao vivo |
| `MochizukiDetailStrength` / `MochizukiColourStrength` | 1 / 0 | 0 a 2 / 0 a 4, ao vivo; colour 0 mantém a cor do jogo, 1 aplica a cor da rede |
| `MochizukiPasses` | 1 | 1 a 3, reconstrói (a rede antiga serve durante a construção) |
| `MochizukiModelScale` | 1 | 0,25 a 1 em passos de 0,05, reconstrói; abaixo de 1 ainda não recomendado |
| `MochizukiIntensity`, `MochizukiStyle`, `MochizukiLocalTone`, `MochizukiLocalStructure`, `MochizukiSkinStructure`, `MochizukiAutoMask` | 1, 0, 1, 1, −1, true | controles do modelo, ao vivo |
| `MochizukiMaxRatio` | 2 | 1 a 8, proteção de realces |
| `MochizukiWhitePoint` | 1 | 0,01 a 100, só cor linear, só no INI |
| `MochizukiApplyModel` | true | false: roda a rede e mostra o quadro original |
| `MochizukiLinearInput` | 0 | 0 auto, 1 sim, 2 não; reconstrói |
| `MochizukiDynamicResolution` (onda 2) | `auto` | `auto`, `exact`, `always` |
| `MochizukiPass2*` e `MochizukiPass3*` (12 chaves) | `auto` | herdam o passe 1, com local tone 0 |

Variáveis de ambiente do runtime (não são chaves do INI): `MZ_COMPILE_THREADS` (threads do prewarm; padrão
min(8, núcleos − 2); 1 desliga), `MZ_PROBE_PIPELINE_BINARY=1` (sonda da chave de pipeline binary) e os ganchos de
falha `MZ_TEST_*` usados pelo `mz_stress` (lidos uma vez, sem custo quando ausentes).

Menu (Neural → "DLSS Neural Rendering", runtime mochizuki): avisos de `ApplyAfterRR` (com botão "Run before Super
Resolution") e de Ray Reconstruction; Temporal ("Temporal history", "History strength"); Effect ("Passes", "NR
resolution (% of render)", ambos ao soltar; "Detail strength", "Colour strength", "Highlight guard"); Model
("Style", "Intensity", "Local structure", "Local tone", "Skin structure", "Auto skin mask", nó "Pass 2/3");
Advanced ("Linear input", "Dynamic resolution" (onda 2), "Network only (no effect)"); Status (extensão da rede,
tempo de GPU mediana e p95, última construção, uso do histórico, vetores recusados, "Failed: ..."). O aviso e o
botão de `AmdDynamicScale` saíram na onda 2.

### 11.3 Estabilidade

| Problema (24/09) | Correção | Pacote |
|---|---|---|
| Falha ao iniciar a sessão vazava a instância Vulkan e referências D3D12 a cada frame, sem espera | início desfeito sem vazamento; host espera de 2 a 60 s entre tentativas; `[unsupported]` registrado uma vez | S1, H1 |
| Fences `done[]` ficavam sinalizadas depois de um `Drain` | `vkResetFences` | S1 |
| Sair do jogo durante a construção travava até ~55 s | na saída do processo o host não chama `Destroy` nem `FreeLibrary`; fora da saída, `Destroy` espera no máximo 2 s e a construção termina sozinha; o núcleo pode parar entre pipelines guardando o cache | H1, S4 |
| Qualquer erro desligava o NR até o fim do processo | só a perda do device é definitiva; um quadro que falha mostra a cor original (nunca preto); construção que falha tenta de novo | S2 |
| Nenhuma checagem de VRAM | admissão antes de construir (D3DKMT), "insufficient VRAM", nova tentativa a cada 10 s; a rede é liberada se os buffers do quadro não couberem | S2 |
| Fence única nos dois sentidos podia andar para trás; a fila do jogo ficava presa se o Vulkan morresse | duas fences; watchdog (5 s sem progresso); filas do jogo liberadas na perda do device | S3 |
| Troca de fila destruía a sessão na thread de submit; `Retire` fora do lock | `ANY_QUEUE` (sem migração, o frame da fila nova espera o último da anterior); `Retire` sob lock; handles de job com geração | H1, S2, S3 |
| Troca de resolução drenava a fila do jogo na thread de render (21-31 ms) | 0,4-0,6 ms; rede antiga liberada por uma thread; passes e escala reconstroem com a rede antiga servindo; segunda fila Vulkan para construir | S4 |
| Resolução dinâmica reconstruía a rede a cada mudança | balde em `auto`: 0 reconstruções, NR em 100% dos quadros no teste | S5 |
| Recursos liberados sob uma lista órfã ainda não executada | "cemitério" liberado depois | S2 |
| Job pendente de uma lista nunca executada desligava o NR | cancelado no `Reset`/`Release` da lista | S8 |
| Caminho com caracteres fora do ANSI fazia a construção falhar | raiz em UTF-8 | S1 |
| `frameId` avançava em chamadas recusadas (histórico zerado a cada quadro com `DlssNrPreUpscale`) | deduplicado | H1 |
| Barreiras de XeSS, FSR 2.2, FSR 2.1.2 e FSR 3.1 mexiam na textura de saída do NR | não mexem mais | S6 |
| Hooks Vulkan do OptiScaler na instância e no device do runtime (Proton, overlays) | passam direto | S7 |
| `ApplyAfterRR` punha o mochizuki depois do SR; `AmdModelScale`/`AmdDynamicScale` zeravam o histórico | ignorados para o mochizuki | C2, S5 |

### 11.4 Desempenho (RX 9070 XT, 26.8.1; mesma sessão, pares intercalados)

| Medida | 0.3.0 (24/09) | Onda 1 | Onda 2 | Total |
|---|---|---|---|---|
| Intervalo da rede na fila do jogo (`mz_timing`), 1080p | 10,23 ms | 9,80 ms | **9,39 ms** | −0,86 ms (−8,4%) |
| O mesmo, 1440p | 17,32 ms | 16,76 ms | **16,10 ms** | −1,23 ms (−7,1%) |
| Só a rede (`nr_graph --per-layer`), 1080p | 9,69 ms | 9,27 ms | **8,89 ms** | −0,80 ms |
| O mesmo, 1440p | 15,82 ms | 15,26 ms | **14,65 ms** | −1,17 ms |
| Status "network", 1080p / 1440p | 9,80 / 16,70 ms | 9,31 / 16,14 ms | 8,9 / 15,5 ms | |
| Primeira construção no harness | 24,8 s | 23,2-23,7 s | **5,8-6,0 s** com o manifesto; 21,6 s sem | −76% |
| `PrepareFrame` na troca de resolução | 31,0 / 21,6 ms | igual | **0,44 / 0,57 ms** | |
| CPU do `EnqueueHip` | 0,098 ms | igual | igual | |

- De onde vem o ganho da rede, sempre com saída idêntica: P4 (onda 1: pesos e4m3 lidos em palavras), P6
  (`fswin32` com `NR_FWAVES=2`; `fswinfusedup128` com `NR_EXPAND_GROUP=2`) e P9 (conversões e4m3 em quádruplas,
  `NR_QUAD`: `s_setreg` 7134 → 4158; blocos V em RowMajor, `NR_V_ROW`: `ds_load_u8` 512 → 0).
- Primeira construção: 32 pipelines compilados em 8 threads em ~4,3 s a partir de
  `dlssnr-amd/prewarm/manifest.txt`; o cache passou a incluir adaptadores e temporais (P3). No jogo não foi medido.
- Não adotado: P7 (`--private-index` constrói em 18,6 s, mas a rede fica +3,3 ms mais lenta e a saída muda); P18
  (o `NR_CHAIN` do upstream estoura o tempo em todo quadro; as formas seguras custam +0,2 a +0,8 ms; persistência em
  C=32 é mais lenta e errada em 4K); N1 do P6 (`fswinpds256` sem spill, mas 12,7% mais lento).
- O número do status do `mz_phases` varia muito entre sessões (16 ou 25 ms em 1440p para a mesma DLL): é o
  clock da GPU nesse harness, como no `mz_pan`. Compare só `mz_timing`, `mz_bench` e `nr_graph`.

### 11.5 Ferramentas novas (em `exports/`, fora do git)

- `mochizuki-harness/`: `mz_bench`, `mz_phases` (tokens de resolução dinâmica `alloc=` e `sub=`), `mz_pan`,
  `mz_timing` (tempos na fila D3D12: seg1, intervalo, seg2), `mz_stress` e `mz_compare.py`; todos aceitam
  `--ctl`, caminhos UTF-16 e `--dump-dir`. `mz_stress` roda cada caso num processo filho: drain, drainval,
  cancel, leak, leakfail, unsupported, enumfail, build-destroy, rebuild-serve, busy, retire-late, orphan,
  timeout, enqthrow, oombuild, vram, vramband, lost, xq, drop e rerelease (`all`, 21 casos); `drspad` só pelo nome.
  Com `MZ_STRESS_DRS_MODE=1` (auto) ou `2` (always) cada sessão recebe esse `drs_mode` e o `busy` espera que a
  mudança de subrect mantenha a rede (adotado da cópia do S5 no pacote RL2, 25/09).
- `mochizuki-work/golden/`: saídas de referência de 24/09 (bit a bit), tempos, construção a frio e o
  resultado do `mz_stress` na 0.3.0.
- `mochizuki-work/tools/`: `mz_equiv.py` (equivalência exata num comando, ~30 s), `mz_perf.py` (pares
  intercalados com `--baseline`), clang-format 20.1.8 (venv `cf`) e RGA.
- `mochizuki-work/isa/` (kit de ISA: `dump-isa.cmd`, `isa_count.py`), `sweeps/` (P2), `nrgraph/` (`nr_graph`
  standalone e planos), `P4/ngbench.py` (`nr_graph` intercalado com checagem de imagem).
- No repositório: `tools/sweep_mochizuki_knobs.py` (varredura de knobs de `pipelines.json`).
- Procedimentos exatos (locks, builds, equivalência, desempenho, construção a frio, stress):
  `exports/mochizuki-work/PROCEDURES.txt`.

### 11.6 Patches no upstream (`third_party/mochizuki/UPSTREAM.md`)

1. `nr_runtime.cpp`: cast para `VkImageAspectFlags` (24/09).
2. `fswin_t.comp`: leituras dos pesos e4m3 e gravações das execuções persistentes em palavras (P4).
3. `pipelines.json`: `fswin32` `NR_FWAVES` 1 → 2 e `fswinfusedup128` `NR_EXPAND_GROUP` 4 → 2 (P6).
4. `fswin_t.comp`, `include/coopmm.glsl` e `pipelines.json`: `NR_QUAD` e `NR_V_ROW` (P9).
5. `nr_runtime.hpp` e `nr_graph.cpp`: `nr::build_cancel`, parada da construção entre pipelines, gravando o cache
   antes (S4).

Seções só de registro: "## Build" (P7: variantes de unroll e `spirv-opt`, nenhuma adotada) e "## Barriers and
`NR_CHAIN`" (P18: medições, candidatos de fusão, e o limite de 16 bits do índice da fila persistente). Os patches
2 a 4 são candidatos a PR no upstream (saída idêntica; não conferidos em RADV/ACO).

### 11.7 O que falta

**Testes em jogo (usuário):** a lista completa, em português, está em `exports/mochizuki-work/wave2-report.md`
§7 (29 itens) e junta as pendências da onda 1 às da onda 2. Os pontos principais: primeira construção com o
manifesto e saída durante a construção; controles ao vivo; passes e resolução reconstruindo sem buraco sem NR;
trocas de resolução sem engasgo; um jogo com resolução dinâmica; alt-tab; VRAM no limite; nenhum "watchdog" no
log em jogo normal; barreiras dos upscalers com a camada de debug do D3D12 (S6).

**Pendências abertas:**
- Dispensas de critério: registradas em `exports/mochizuki-work/release/WAIVERS.md` (pacote RL2, 25/09): C1
  (escala 0,5; tremulação em 0,75, correção adiada para o C3), P3 (adaptadores em 0,08 s na segunda execução,
  contra 0,05; exceção à DECISIONS 7), S5 (faixa de ±30% em 2304x1296; a saída do balde é idêntica à da rede exata
  no quadro com bordas estendidas; falta o relatório do Q1) e M0. O usuário pode revogar qualquer uma.
- Textos corrigidos pelo RL2 (25/09, só comentários): `drs_mode` em `MochizukiNrControls.h` (os modos 0, 1 e 2), a
  faixa de cor em `LmxxfNrApi.h:87` (0 a 2 no lmxxf, 0 a 4 no mochizuki) e um comentário em `LmxxfEvaluateCut.h`
  explicando que o "output zeroed" da mensagem de troca de fila vale só para o lmxxf (a mensagem em si, que vai
  para o `OptiScaler.log`, não mudou). Continua desatualizado o "about half a minute" do menu e do status,
  anterior ao prewarm.
- `PROCEDURES.txt` §8 e §9 descrevem agora os 21 casos, o `xq` obrigatório, os casos do S1 ao S5, os tokens de
  resolução dinâmica do `mz_phases` e as checagens de chaves e padrões do S5; `build-mochizuki-runtime-stats.cmd`
  (em `exports/mochizuki-work`) linka de novo (patch do P3 aplicado pelo RL2).
- Estimativa de VRAM calibrada numa só GPU; uma falha de `CreateGeometry` por outro motivo mantém a rede
  residente; uma rede aposentada espera o próximo quadro sem limite de tempo (0,3 a 0,9 GB se o jogo parar de
  produzir quadros).
- Uma construção por vez no processo; `Destroy` 1 s depois do início de uma construção a frio devolve em 2,0 s
  (limite 2,5).
- Margens estreitas no harness (wave2-final e as três rodadas limpas do RL2): `oombuild` pronto em 6,6-6,9 s
  (limite 7), `drop` e `rerelease` em 5,2-5,5 s (limite 6).
  Com o Cyberpunk aberto na mesma máquina, o `oombuild` levou 7,5 s e falhou (RL2, 25/09): rode o `mz_stress` com
  a máquina quieta.
- Limite de 16 bits da fila persistente (S9, à parte) e fusões candidatas do P18.
- Nada testado em jogo: TDR real, troca de fila do jogo, D3DKMT sob o spoofing do OptiScaler, primeira construção
  no jogo com o manifesto.
