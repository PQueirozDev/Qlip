# Qlip

Gravador de tela e de clipes instantâneos para Windows 10/11, nativo (C++20, Win32), focado em ficar leve
enquanto um jogo roda. Alternativa minimalista ao OBS e ao ShadowPlay. Open source (MIT).

**Site:** https://qlipapp.vercel.app · **Download:** [última versão](https://github.com/PQueirozDev/Qlip/releases/latest)
(`Qlip-Setup.exe`, instalador sem pedir administrador, ou `Qlip-win64.zip`, portátil: extraia e abra o `Qlip.exe`).

## Funcionalidades

- **Gravação contínua** (F9): tela inteira (qualquer monitor, escolhido nas configurações) ou só o jogo
  (janela em primeiro plano).
  H.264 por NVENC, saída com FPS constante (30/60/120), resolução original, 1080p ou 720p, 4 níveis de qualidade.
- **Replay instantâneo** (F10 liga/desliga, F8 salva): mantém os últimos 15/30/60/120 s em memória como pacotes
  já codificados (nunca frames brutos) e salva o clipe em MP4, começando em keyframe. Roda junto com a gravação.
- **Áudio**: som do sistema (WASAPI loopback) e microfone (padrão do Windows ou um escolhido), cada um com volume
  próprio, mixados em AAC e sincronizados com o vídeo pelo mesmo relógio (QPC). Se o dispositivo for desconectado
  ou o padrão do Windows mudar, a captura reconecta sozinha; o intervalo sem dispositivo vira silêncio.
- **Painel no jogo** (Alt+P): abre por cima do jogo, como o Alt+Z do app da NVIDIA, com replay, salvar clipe,
  gravar, biblioteca e configurações. Escurece só o monitor do jogo, devolve o foco ao fechar (Esc ou Alt+P) e não
  aparece nas gravações nem nos clipes. Funciona com jogos em janela ou tela cheia sem borda.
- **Biblioteca**: lista gravações e clipes (duração, tamanho, data); abrir, mostrar na pasta, excluir (para a lixeira).
- **Configurações**: pastas, FPS e qualidade separados para gravação e clipe (escolhidos em listas suspensas),
  atalhos personalizáveis com Ctrl/Alt/Shift,
  inicialização com o Windows. Salvas em `%APPDATA%\Qlip\settings.json`.
- **Recuperação**: a gravação é escrita em MP4 fragmentado temporário; se o app cair, o arquivo é recuperado na
  próxima abertura (perde-se no máximo o último fragmento, ~1 s).

Gravações vão para `Vídeos\Qlip`; clipes, para `Vídeos\Qlip\Clipes`. Log em `%APPDATA%\Qlip\logs\qlip.log`.

## Requisitos

- Windows 10 2004+ ou Windows 11 (Windows Graphics Capture).
- GPU NVIDIA com NVENC e driver atualizado. Testado na RTX 2060 (H.264; AV1 não é usado).

## Compilar

Requer Visual Studio 2022 Build Tools (C++ desktop), CMake 3.24+ e PowerShell 7.

```
pwsh scripts/fetch-ffmpeg.ps1
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

O primeiro comando baixa o FFmpeg (headers, libs e DLLs) para `third_party/ffmpeg`, que não fica no repositório.

O executável fica em `build\Release\Qlip.exe`, com as DLLs do FFmpeg copiadas ao lado.
O FFmpeg usado é a build **LGPL shared** em `third_party/ffmpeg` (ver `THIRD_PARTY.md`).

**Distribuição**: `pwsh scripts/build-release.ps1` gera `dist\Qlip-portable` (exe + 5 DLLs do FFmpeg + licenças),
`dist\Qlip-win64.zip` e o instalador `dist\Qlip-Setup.exe` (requer Inno Setup 6; script em `installer\Qlip.iss`).
O instalador põe o app em `%LOCALAPPDATA%\Programs\Qlip`; desinstalar mantém as configurações (`%APPDATA%\Qlip`)
e os vídeos.

## Resultados medidos

Máquina: Windows 11, i7-10700F (16 threads), RTX 2060 6 GB (driver 610.88), SSD NVMe. Build Release x64.
Carga de captura: padrão de teste em movimento (`testsrc2`, 1920×1080 a 60 fps, tela cheia) — **não foi medido
com jogo real**. CPU é do processo do Qlip normalizada para todas as threads; a utilização da GPU inclui o
programa que exibia o padrão de teste. Qualidade média (12 Mbps), 1080p60, áudio do sistema ligado.

| # | Teste | Resultado |
|---|---|---|
| 1 | Gravação de 10 min em 1080p60 | 603,5 s, 36.187 frames (60 fps), 682 MB, 19 frames perdidos (0,05%), arquivo reproduzível. Timestamps tinham jitter de 1 ms (intermediário Matroska), corrigido depois; a correção foi verificada em gravações de 2 min (CFR exato, 0 erros), **o teste de 10 min não foi refeito** |
| 2 | Replay de 30 s e 120 s | 120 s → clipe de 120,4 s; 30 s → clipes de 30,5–31,1 s; 0 erros de decodificação |
| 3 | Replay durante gravação | Clipe de 30,8 s + gravação de 120 s, ambos com 0 erros |
| 4 | Clipes consecutivos | 3 clipes com F8 seguidos (15,1–16,0 s), 0 erros |
| 5 | Microfone + áudio do sistema | Áudio do sistema verificado (sinal médio −22,7 dB). **Microfone não testado** |
| 6 | Alt+Tab durante jogos | **Não testado** (sem jogo no ambiente de teste) |
| 7 | Encerramento com replay ativo | Saída em 0,3 s com código 0, gravação finalizada (0 erros), nenhum processo restante |
| 8 | Inicialização sem GPU compatível | **Não testado** (a máquina tem GPU compatível) |
| 9 | Repouso | RAM 21,9 MB (privada 18,8 MB), CPU 0,00%, NVENC 0% |
| 10 | Durante a captura | Gravação: RAM 247 MB (privada 371 MB), CPU 2,7%, NVENC 11%. Replay 120 s: RAM até 411 MB (buffer cheio). Gravação + replay: RAM 458 MB (privada 712 MB), CPU 5,5%, NVENC 23% |

Outros pontos medidos:

- **Crescimento de memória** em 10 min de gravação: 243,8 → 249,2 MB (0,56 MB/min), sem sinal de vazamento.
- **Origem da memória durante a captura**: ~220 MB são da sessão NVENC/driver NVIDIA. Medido com o `ffmpeg.exe`
  das mesmas DLLs: 346 MB de memória privada com `h264_nvenc` contra 124 MB com encoder de software. Reduzir
  superfícies do NVENC não mudou esse valor.
- **Recuperação após queda**: 3 quedas em momentos diferentes, os 3 arquivos recuperados com 0 erros.
- **Impacto em FPS de jogos**: não medido.

## Limitações conhecidas

- O primeiro ~0,2–0,5 s de cada gravação mostra o primeiro frame parado: é o tempo que o Windows Graphics Capture
  leva para entregar a primeira imagem. A sincronia com o áudio não é afetada.
- Gravação e replay ligados juntos abrem duas sessões NVENC (~2× a memória).
- Captura de conteúdo protegido (DRM) não é possível e não é contornada.
- O áudio do sistema sempre vem do dispositivo de reprodução padrão (o microfone pode ser escolhido).
- "Só o jogo" grava a janela em primeiro plano no momento do F9; não há lista para escolher.
- A reconexão de áudio após desconectar o dispositivo ou trocar o padrão do Windows não foi testada com troca real
  de dispositivo.
- Na biblioteca, com muitos arquivos, a barra de rolagem vertical usa o estilo clássico do Windows (cinza).
- Configurações de versões anteriores (pasta `%APPDATA%\QClip`) são movidas para `%APPDATA%\Qlip` na primeira execução.
- O painel no jogo não abre por cima de jogos em tela cheia **exclusiva**: o Windows minimiza o jogo ao trocar o
  foco. Use tela cheia sem borda (padrão na maioria dos jogos atuais). Não foi testado com um jogo real.
- O instalador e o executável não são assinados; o SmartScreen pode avisar na primeira execução.

## Licença

MIT (ver `LICENSE`). O FFmpeg é distribuído como DLLs separadas sob LGPL; detalhes em `THIRD_PARTY.md`.
