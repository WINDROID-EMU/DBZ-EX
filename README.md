# Dragon Ball Z: Extreme Butoden - Native Android Port (Static Recompilation)

[![Platform](https://img.shields.io/badge/Platform-Android%20(ARM64)-brightgreen.svg)]()
[![Graphics](https://img.shields.io/badge/Graphics-OpenGL%20ES%203.0%20%2F%20PICA200-blue.svg)]()
[![Kernel](https://img.shields.io/badge/Kernel-Horizon%20OS%20HLE-orange.svg)]()

Port nativo em C++ / ARM64 de **Dragon Ball Z: Extreme Butoden (Nintendo 3DS)** para Android, utilizando **Recompilação Estática (Static Recompilation)** através do `3dsrecomp` e um subsistema HLE do **Horizon OS** integrado diretamente a um pipeline de renderização **PICA200 GPU** em OpenGL ES 3.0.

---

## 🌟 Visão Geral / Overview

Diferente de emuladores tradicionais (que interpretam ou usam recompilação dinâmica JIT em tempo de execução com alto overhead de CPU), este projeto recompilou antecipadamente todo o código de máquina ARM11 do jogo original em código C nativo otimizado. 

O runtime Android executa o código recompilado de forma nativa a 60 FPS, gerenciando:
1. **Horizon OS Kernel HLE**: Threads cooperativas/preemptivas, Árbitros de Endereço (Address Arbiters / Futex), Eventos de Sincronização e canais IPC (`gsp:Gpu`, `apt:U`, `dsp::DSP`, `fs:USER`, `hid:USER`).
2. **PICA200 GPU Command Processor**: Processamento nativo de comandos de desenho, buffers de vértices 3D, index buffers, e matrizes de projeção.
3. **Rasterizador Baricêntrico com Decodificação ETC1/ETC1A4**: Rasterizador com suporte a blend de transparência alfa e decodificação precisa dos formatos de textura do Nintendo 3DS (ETC1 e ETC1A4 em sub-blocos 4x4 invertidos).
4. **Apresentação Dual-Screen (EGL / OpenGL ES 3.0)**: Renderização das telas superior (400x240) e inferior (320x240) na mesma superfície Android sem sobreposição de framebuffers.

---

## 📁 Estrutura do Projeto / Project Structure

```
├── android/                   # Projeto Android Studio completo
│   ├── app/
│   │   ├── src/main/
│   │   │   ├── cpp/           # Runtime C++ Nativo (Horizon OS, PICA200, EGL)
│   │   │   │   ├── horizon/   # Kernel Horizon OS HLE (threads, IPC, mem)
│   │   │   │   ├── gpu/       # PICA200 GPU rasterizer, ETC1, display transfer
│   │   │   │   ├── recomp/    # Headers do ABI de recompilação
│   │   │   │   └── main_jni.cpp # Ponto de entrada JNI e loop principal
│   │   │   ├── java/          # Camada Java (SurfaceView, AudioTrack, Input)
│   │   │   └── AndroidManifest.xml
│   │   └── build.gradle
│   ├── build.gradle
│   └── gradlew
│
├── recomp_out/                # Código C recompilado estaticamente (36 unidades)
│   ├── code000.c ... code035.c
│   ├── entries.c              # Tabela de endereços de funções (lookup binário)
│   ├── functions.h
│   └── recomp.h
│
├── 3dsrecomp/                 # Ferramenta estática de recompilação ARM11 -> C
└── .gitignore                 # Filtro de ROMs, logs e arquivos de build
```

---

## 🛠️ Como Compilar / How to Build

### Pré-requisitos
- **Android SDK** & **Android NDK** (r25c ou superior)
- **CMake** 3.22+
- **JDK 17**
- Dispositivo Android físico com arquitetura **ARM64-v8a** (Android 9.0+)

### Passos de Compilação
1. Clone o repositório com os submódulos:
```bash
git clone --recurse-submodules https://github.com/WINDROID-EMU/DBZ--EX.git
cd DBZ--EX
```

2. Compile o APK de depuração via Gradle:
```bash
cd android
./gradlew assembleDebug
```

3. Instale e execute no dispositivo conectado via ADB:
```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.dbz.butoden/.MainActivity
```

4. Acompanhe os logs de execução:
```bash
adb logcat -s DBZ-Horizon DBZ-GPU DBZ-Native
```

---

## ⚖️ Aviso Legal / Legal Disclaimer

Este repositório contém estritamente o código-fonte de engenharia reversa, runtime de compatibilidade e ferramentas de recompilação estática. **Nenhum arquivo proprietário de jogo, ROM, dump de RomFS, ExHeader ou ativo com direitos autorais da Nintendo ou Bandai Namco está incluído ou hospedado neste repositório**. 

Para rodar o projeto, o usuário deve possuir uma cópia legítima do jogo e extrair os assets utilizando ferramentas próprias de acordo com as leis de cópia de segurança de sua jurisdição.
