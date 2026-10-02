# Prompts para migrar o render do DBZ Butoden (3DS) para a GPU Adreno

Como usar:
1. Abra o Claude Code (ou outra IA com acesso ao repositório) na pasta do projeto clonado.
2. Cole o **PROMPT MESTRE** uma vez, no início da sessão.
3. Depois cole **um comando de fase por vez** (Fase 0, 1, 2...). Só avance quando a fase anterior compilar e rodar.
4. Se algo der errado, use o **PROMPT DE DEPURAÇÃO** no fim.

Repositório: https://github.com/WINDROID-EMU/DBZ-EX

---

## PROMPT MESTRE (colar uma vez)

```
Você vai migrar o renderer 3D do port Android de Dragon Ball Z: Extreme Butoden (3DS) de um rasterizador em software (CPU) para renderização nativa na GPU Adreno via OpenGL ES.

## Contexto do projeto
Repositório: DBZ-EX (pasta atual). Port nativo Android ARM64 por recompilação estática (recomp_out/*.c) com HLE do Horizon OS. Código de GPU em android/app/src/main/cpp/gpu/. NDK r25c+, CMake 3.22+, ABI arm64-v8a.

## Estado atual (leia estes arquivos antes de qualquer mudança)
- gpu/pica_cmd_processor.cpp: ProcessCommandList() interpreta a lista de comandos do PICA200 (2 palavras por comando: valor + header; reg = bits 0..9, máscara = bits 16..19, extras = bits 20+, bit 31 = consecutivo) e guarda tudo em m_regs[0x400]. Escrita em GPUREG_DRAWARRAYS (0x22E) / GPUREG_DRAWELEMENTS (0x22F) dispara o desenho.
- ExecuteDrawElements() é um RASTERIZADOR EM SOFTWARE: lê vértices (posição float3 no buffer 0, cor float4 no buffer 1, UV float2 no buffer 2, layout hardcoded), adivinha o espaço de coordenadas (heurística isScreenSpace), rasteriza por coordenadas baricêntricas, amostra textura por vizinho mais próximo (RGBA8 Morton, ETC1=12, ETC1A4=13 via rg_etc1) e escreve direto no framebuffer emulado (retrato 240x400 / 240x320, Morton 8x8). ExecuteDrawArrays() só loga.
- gpu/pica_display_transfer.cpp: ExecutePicaMemoryFill / ExecutePicaDisplayTransfer (deswizzle, conversão de formato, flip, escala 2x) e depois os->NotifyFramebufferUpdated().
- horizon/horizon_os.cpp: GSP cmd 1 (SubmitCommandList), 2 (MemoryFill), 3 (DisplayTransfer). NotifyFramebufferUpdated() gira 240x400 -> 400x240 RGBA em m_topFrameRGBA/m_botFrameRGBA.
- gpu/pica_gles.cpp: só APRESENTA duas texturas (400x240 e 320x240) em dois viewports. gpu/egl_manager.cpp: contexto GLES.
- main_jni.cpp: loop de ~60 Hz, dispara VBlank (PDC0/PDC1), chama RenderFrame e SwapBuffers.

## Objetivo
Substituir o rasterizador em CPU por renderização na Adreno, mantendo o parser de comandos e m_regs[] como estão. A tela final continua saindo pelo PicaGLES.

## Alvo de hardware
Dispositivo de desenvolvimento: GPU Adreno com driver OpenGL ES 3.2. Use isso, mas sem tornar o 3.2 obrigatório:
- No EGLManager, peça contexto 3.2 (EGL_CONTEXT_MAJOR_VERSION=3, EGL_CONTEXT_MINOR_VERSION=2) e, se falhar, faça fallback para 3.0. Registre GL_VERSION, GL_RENDERER e a lista de extensões no log de inicialização.
- Crie uma struct de capacidades (ex.: GpuCaps) preenchida na inicialização (computeShaders, copyImage, framebufferFetch etc.). Todo uso de recurso 3.1/3.2 passa por ela, com caminho alternativo equivalente para 3.0.
- Shaders gerados: "#version 320 es" no contexto 3.2, "#version 300 es" no fallback; precision highp float onde o PICA exigir (posição e depth).
- Não use geometry nem tessellation shaders.

## Regras de trabalho
- Faça UMA fase por vez, só quando eu mandar. Ao fim de cada fase: compile (cd android && ./gradlew assembleDebug), descreva o que mudou, o que foi validado e o que ficou pendente.
- Mantenha o rasterizador antigo atrás de uma flag (PICA_RENDERER_GPU) até a Fase 5. Não apague nada antes disso.
- Não adicione ROMs, dumps de RomFS ou assets proprietários ao repositório.
- Logs verbosos só com flag de debug. Nada de log por pixel ou por triângulo no caminho quente.
- Comente em português as partes não óbvias (formatos de registrador, conversões de coordenada).
- Se algo depender de comportamento do hardware que você não consegue confirmar só pelo código, diga explicitamente em vez de assumir.
- Logs úteis no aparelho: adb logcat -s PicaCmdProc PicaGLES PicaTransfer EGLManager

Confirme que entendeu e aguarde o comando da Fase 0.
```

---

## FASE 0 — Base e instrumentação

```
Execute a Fase 0.
- Garanta que o projeto compila com ./gradlew assembleDebug.
- Adicione a flag de build/runtime PICA_RENDERER_GPU para alternar entre o rasterizador antigo e o novo (padrão: antigo).
- Reduza o spam de LOGI dentro de ExecuteDrawElements e ExecutePicaDisplayTransfer (hoje há logs e ReadBytes pesados no caminho do draw), deixando-os atrás de uma flag de debug.
- Crie a struct GpuCaps e o log de inicialização (GL_VERSION, GL_RENDERER, extensões) no EGLManager/PicaGLES, com contexto 3.2 e fallback 3.0.
Ao final, me diga o que mudou e como validar no aparelho.
```

## FASE 1 — Render em FBO (UI 2D primeiro)

```
Execute a Fase 1.
- Crie FBOs GLES (cor RGBA8 + depth) para a tela de cima e a de baixo, com resolução interna configurável (1x nativa por padrão; deixe pronto para 2x/3x).
- Com PICA_RENDERER_GPU ligada, o draw escreve no FBO em vez de nos bytes da memória emulada. Isole em uma função a decisão de qual tela usar (hoje: colorBufPhys >= 0x18200000).
- PicaGLES::RenderFrame apresenta a textura do FBO diretamente, sem passar por m_topFrameRGBA/m_botFrameRGBA.
- Sincronização: quando o jogo ler o framebuffer de volta (DisplayTransfer/MemoryFill), garanta coerência com glReadPixels + swizzle Morton + formato correto, ou um cache com flag "conteúdo na GPU / na RAM". Use glCopyImageSubData quando GpuCaps permitir.
- Por enquanto, o draw pode continuar usando posições/UV/cores do layout atual (hardcoded) e blend simples; o foco é mover o destino do desenho para a GPU.
Critério: menus e UI 2D que hoje aparecem continuam aparecendo, agora vindos do FBO. Reporte o tempo por frame antes e depois.
```

## FASE 2 — Texturas na GPU

```
Execute a Fase 2.
- Cache de texturas GL indexado por (endereço, largura, altura, formato, hash do conteúdo ou flag de invalidação).
- Deswizzle Morton 8x8 e conversão de formato. Formatos do PICA: RGBA8, RGB8, RGB565, RGBA5551, RGBA4444, L8, A8, LA8, HILO8, L4, A4, LA4, ETC1 (12), ETC1A4 (13). Hoje só RGBA8/ETC1/ETC1A4 existem e qualquer outro tipo cai no ramo RGBA8 (bug).
- ETC1: o 3DS guarda os 8 bytes do bloco invertidos. Reordene e suba como GL_COMPRESSED_RGB8_ETC2 (compatível com ETC1) ou decodifique com rg_etc1 e suba RGBA. ETC1A4 tem 8 bytes de alfa (nibbles, ordem Morton) + 8 bytes de ETC1.
- VERIFIQUE a ordem dos tiles no ETC1: o código atual usa (tileX * tilesPerCol + tileY); o padrão do 3DS costuma ser linha primeiro (tileY * tilesPerRow + tileX). Confirme com um dump de textura conhecida antes de assumir.
- Wrap/filtro/border a partir de GPUREG_TEXUNIT0_PARAM; flip vertical (origem da textura do 3DS é embaixo-esquerda).
- Se GpuCaps.computeShaders estiver disponível, faça deswizzle, inversão de bytes do ETC1 e conversão de formato em compute shader, mantendo o caminho em CPU como fallback e para comparar a saída nos testes.
Critério: todas as texturas de UI e sprites com formatos diferentes de RGBA8 aparecem corretas.
```

## FASE 3 — Geometria e atributos

```
Execute a Fase 3.
- Leia GPUREG_ATTRIBBUFFERS_FORMAT_LOW/HIGH e ATTRIBBUFFERn_CONFIG1/2 para montar o layout real (tipos byte/short/float, nº de componentes, stride, permutação de atributos) em vez do layout fixo.
- Suba os buffers para VBO/IBO (índices de 8 ou 16 bits, bit 31 de INDEXBUFFER_CONFIG) e use glDrawElements/glDrawArrays reais.
- Respeite GPUREG_PRIMITIVE_CONFIG (triangles, strip, fan) e GPUREG_VERTEX_OFFSET. Implemente ExecuteDrawArrays de verdade (hoje só loga).
- Ignore o olho direito do 3D por enquanto.
Critério: a geometria da UI e dos sprites continua correta usando o layout lido dos registradores.
```

## FASE 4 — Shaders

```
Execute a Fase 4. Esta é a fase mais pesada; trabalhe em sub-etapas (4a vertex, 4b fragment) e me reporte ao fim de cada uma.

4a) Vertex shader
- O jogo envia bytecode PICA pelos registradores VSH_CODETRANSFER_CONFIG/DATA, VSH_OPDESCS_CONFIG/DATA, VSH_FLOATUNIFORM_CONFIG/DATA, VSH_BOOLUNIFORM, VSH_INTUNIFORM, VSH_ENTRYPOINT, VSH_OUTMAP_*. Capture esse estado, traduza o bytecode para GLSL ES e compile com cache por hash (bytecode + opdescs + outmap). Para instruções ainda não suportadas, registre o opcode no log.
- Aplique a conversão de clip space do PICA (z em [-1,0], viewport via VIEWPORT_WIDTH/HEIGHT/XY e DEPTHMAP_SCALE/OFFSET) para o clip space do GL.
- Remova a heurística isScreenSpace assim que as matrizes do jogo estiverem sendo aplicadas pelo vertex shader.

4b) Fragment shader
- Gere GLSL dinamicamente a partir dos 6 estágios TexEnv (TEXENV0..5 SOURCE/OPERAND/COMBINER/COLOR/SCALE, TEXENV_BUFFER_COLOR, UPDATE_BUFFER), com cache por configuração. Comece por REPLACE e MODULATE (suficiente para a UI) e depois ADD, ADD_SIGNED, INTERPOLATE, SUBTRACT, DOT3 etc.
- Iluminação por fragmento (LIGHTING_*, LUTs), fog e shadow ficam como stubs claros por enquanto.
Critério: geometria 3D (lutadores, cenário) aparece na posição certa, sem depender de isScreenSpace.
```

## FASE 5 — Estado de pipeline e limpeza

```
Execute a Fase 5.
- Blend (COLOR_OPERATION, BLEND_FUNC, BLEND_COLOR), alpha test (FRAGOP_ALPHA_TEST, via discard no fragment shader), depth test e máscara (DEPTH_COLOR_MASK), stencil (STENCIL_TEST/OP), scissor (SCISSORTEST_*), culling (FACECULLING_CONFIG).
- Logic op não existe em GLES core: emule no fragment shader com GL_EXT_shader_framebuffer_fetch se GpuCaps.framebufferFetch estiver disponível; senão documente a limitação.
- Quando a versão em GPU estiver estável, deixe PICA_RENDERER_GPU ligada por padrão e proponha (sem executar ainda) a remoção do rasterizador de software e do código morto.
Entregue um resumo final com o que está suportado, o que é stub e o que ficou de fora.
```

---

## PROMPT DE DEPURAÇÃO (quando algo der errado)

```
O resultado da fase atual não está correto. Sintoma: <descreva: tela preta / imagem torta / cores trocadas / crash / travamento no VBlank>.

Faça nesta ordem, sem tentar corrigir às cegas:
1. Releia o código alterado nesta fase e liste hipóteses ordenadas por probabilidade.
2. Para cada hipótese, indique qual log ou checagem a confirma ou descarta (glGetError após cada chamada GL relevante, estado do FBO com glCheckFramebufferStatus, log do shader com glGetShaderInfoLog, valores dos registradores do PICA no momento do draw).
3. Adicione só a instrumentação mínima necessária, atrás da flag de debug, e me diga qual comando adb logcat rodar e o que procurar.
4. Só depois de eu devolver os logs, proponha a correção.
```