# Handoff: atualizações do mochizuki0323/DLSSNR-AMD (01/10/2026)

O upstream ganhou 7 commits depois do 4f663b5 (v0.0.2.1, 27/09), até o 51b65a6 (01/10). Este fork usa o
`windows/` dele, com 15 patches nossos por cima (`third_party/mochizuki/UPSTREAM.md`). Os patches não aplicam
direto: o que foi aproveitado foi portado à mão, e está na entrada 16 do `UPSTREAM.md`.

## Feito nesta versão (não commitado, não testado em jogo)

1. **Peso do histórico igual ao do NVIDIA (d1185d2).** O bloco post mistura o quadro anterior com o peso que a
   rede emite vezes o `blend_scale` do modelo, 0,7397. Usávamos 1,0, então carregávamos mais do quadro anterior
   que o original. `kPostBlendScale` em `nr_runtime.cpp`. Muda a imagem em movimento.
2. **Formatos de cor (743326d).** O host aceita R9G9B9E5_SHAREDEXP (RE Engine: Resident Evil Requiem, Monster
   Hunter Wilds), B8G8R8X8 (UNORM, SRGB, TYPELESS), R16G16B16A16 UNORM e SNORM, R8G8B8A8_SNORM e os 16-bit
   empacotados (B5G6R5, B5G5R5A1, B4G4R4A4). Os que só passam por blit só são aceitos onde a GPU faz blit deles
   (`NeedsBlitCheck` + `Session::Blittable`). Antes, um jogo nesses formatos ficava sem NR.
3. **Preprocess (729a05d).** Exposição (auto por histograma, ou fixa), curva, contraste e saturação antes da rede,
   desfeitos na resposta.
   - ABI: campos novos no fim de `MochizukiNrControls` e `MochizukiNrInfo::preprocess_ev`.
   - Chaves `MochizukiPreprocess*` em `[DlssNr]`.
   - Menu do mochizuki, gaveta "Preprocess"; a exposição medida aparece em Info.
   - Desligado por padrão. A primeira vez que liga, a rede é reconstruída uma vez já capaz de rodar o Preprocess
     (`Session::prepWanted`).
   - Shader `runtime_prep.spv` novo em `dlssnr-amd/shaders/runtime/`: o componente mochizuki do instalador precisa
     levá-lo, e o manifesto de prewarm deve ser refeito.
   - O add-on ReShade não expõe o Preprocess (manda a struct curta, o runtime usa os padrões).

Validado: build do runtime e do OptiScaler, testes de contrato do host. **Falta:** rodar o runtime numa GPU
(EQUIV contra os goldens: um quadro sem histórico deve sair igual com o Preprocess desligado; com histórico muda
pelo item 1) e testar em jogo. Os harnesses (`mz_bench`, `mz_phases`, `mz_pan`, `mz_timing`) não estão mais em
`exports/` e precisam ser refeitos ou recuperados.

## Feito em 02/10/2026 (não commitado, não testado em jogo)

A e B abaixo estão feitos, de um jeito mais simples que o sugerido: o `windows/` do upstream (82560c4) já tinha os
dois, então ele entrou inteiro e só dois patches nossos ficaram por cima (o stop flag do build e `NR_EDGE_BODIES=0`,
que leva o build a frio de 109 s para 23 s sem mudar a saída). O resto dos 16 patches antigos o upstream cobre;
a lista está em `third_party/mochizuki/UPSTREAM.md`. O host ganhou `bufferDeviceAddress`, as imagens `SAMPLED`
(leitura no lugar) e `preprocess_unknee`; o `frame_image` saiu.

Medido num harness novo (`mz_e2e` no scratchpad da sessão: imagem real, panorâmica com vetores exatos, mesma
interface do lmxxf): 1080p 9,26 -> 8,50 ms, 4K 55,6 -> 48,1 ms, 3 passes 42,3 -> 38,3 ms; em 1707x961 e 720p
a saída em movimento ficou ~10x mais estável. 2 e 3 passes, R10G10B10A2, RGBA32F, tamanho ímpar, 4K e o
Preprocess rodam sem erro. Lista de prewarm nova em `exports/mochizuki-runtime/dlssnr-amd/prewarm/` (38 linhas,
65 shaders; frio 6,1 s com ela). Para lançar: `pin-mochizuki.ps1` com esse build (shaders mudaram: saiu
`runtime_encode_in.spv`, entraram `runtime_depth.spv`, `noise_field` e outros) e teste em jogo.

## Era para a próxima versão (feito acima)

### A. Movimento como o DLL da NVIDIA (resto do d1185d2)

- O bloco pre e o post leem os vetores de movimento do jogo na resolução cheia, no próprio lugar, em vez de
  uma cópia em 1/4 de resolução que misturava o movimento do primeiro plano com o do fundo em toda silhueta.
  Quando a textura não pode ser amostrada com filtro linear, a cópia antiga continua.
- Subrects de movimento e profundidade que não começam em (0, 0) são respeitados (`runtime_depth.comp` recebe o
  offset). `EngineFrame` ganha `motion_texture_width/height` e `motion_x/y`, `depth_x/y`.
- Ruído do bloco pre: a semente é o número de quadros desde o primeiro ou o último reset, como no DLL; o quadro
  de semente 0 usa o campo pré-calculado, os outros calculam no shader (`noise_field.comp`, `image_noise.glsl`).
- O que ele corrigiu na escala dos vetores (normalizar pela região do movimento) o nosso host já faz certo
  (`MochizukiNrRuntime.cpp`, `next.motionScaleX = info->motion_scale_x / float(motionW)`).
- Medição dele, sequências em movimento contra o DLL da NVIDIA numa RTX 5090: +2 a +5 dB de PSNR sobre o
  0.0.2.4, custo +0,005 ms em 1080p.
- Arquivos: `windows/src/core/nr_runtime.cpp/.hpp`, `windows/shaders/rdna4/fswin_t.comp`,
  `include/image_input.glsl`, `passes/runtime_depth.comp`. Toca nos mesmos arquivos dos nossos patches 12 a 15,
  então é port à mão, com goldens novos.

### B. Rede reconstruída para o compilador AMD do Windows (228d3a6 + b1419b0)

- Números dele, offline numa RX 9070 XT:

  | Resolução | Upstream Windows | Nosso (`nr_graph --per-layer`) |
  |---|---|---|
  | 1080p | 7,21 ms | ~8,5 ms |
  | 1440p | 12,59 ms | ~14,7 ms |
  | 4K | 27,32 ms | ~33,6 ms |

- Ele diz que a versão Windows não foi testada em jogos, e que a imagem difere um pouco da Linux.
- Inclui: barreiras trocadas por contadores por tile, projeções C=512 com pipeline próprio, fold do
  downsample/upsample C=256 em todas as extensões, `fswin_t.comp` dividido em `include/fswin_body.glsl`,
  `quad_quant_glsl.py`, `coopmm.glsl` e `pipelines.json` refeitos, round trips f32->f16->f32 que o compilador AMD
  descartava (`NR_Q32_DIRECT`, `NR_Q32_STAGE`), `HostDevice::buffer_device_address` e builds de rede um de cada vez.
- Caminho sugerido: partir do `windows/` do 51b65a6, reaplicar só o que o nosso host precisa (patch 1, o cast do
  MSVC; 5, o stop flag do build; 12 a 14, as mudanças do runtime) e conferir quais dos nossos ajustes de LLPC
  (2 a 4, 6 a 11) ele já cobre. Medir com `mz_timing` contra o build atual, refazer os goldens, regenerar o
  manifesto de prewarm e testar em jogo antes de publicar.

### Não serve para nós

`windows/src/pe/` (os hosts NGX e ReShade dele, que precisam do vkd3d-proton), inclusive o "in-place" da rota
OptiScaler e a correção de unload do add-on ReShade dele; `linux/`; o instalador dele; o extrator de modelo em
C++ (`dlssnr_extract_model.exe`); os nomes de pacote (648831a, 51b65a6).
