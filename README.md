# UZDoom para Nintendo Switch

Port do [UZDoom](https://github.com/UZDoom/UZDoom) 5.0.3 para o Nintendo Switch
desbloqueado (Atmosphère / Homebrew Menu), com launcher próprio. Roda qualquer mod
que rode no UZDoom 5.0.3 do PC: Brutal Doom, Project Brutality, Lexicon,
Trailblazer, Samsara, WadFusion...

Port, launcher e otimizações para Switch: **Renan** (Telegram @aeon1911).
Baseado no trabalho de fgsfds (port de GZDoom para Switch e launcher original),
Thorhax (GZDoom-NX-2026), da equipe UZDoom/GZDoom/ZDoom e do devkitPro.

## O que é diferente do UZDoom do PC

Os arquivos do motor (`uzdoom.pk3`, `game_support.pk3`, `brightmaps.pk3`,
`lights.pk3`, `game_widescreen_gfx.pk3`) são exatamente os do UZDoom 5.0.3
oficial: nenhum mod é alterado. As mudanças são só para o Switch:

- cache de leitura do cartão SD (abertura de mods grandes muito mais rápida);
- imagens e sons preparados em outro núcleo, prefetch de sprites e mipmaps feitos
  na CPU (sem travadas quando aparecem sprites novos);
- shaders pré-preparados, tela de carregamento com tempo restante;
- 30 FPS travado de verdade (VSync a cada 2 atualizações) e estabilizador de FPS
  (reduz só detalhes visuais - luzes dinâmicas distantes, efeitos longe - quando
  uma cena fica pesada demais);
- luzes dinâmicas com orçamento (as mais próximas iluminam, as outras somem com fade);
- threads do jogo, do áudio e dos workers distribuídos nos 3 núcleos livres;
- `perf.log`, `crash.log` e `hang.log` no cartão para diagnóstico;
- nenhuma mudança de clock (CPU/GPU sempre nos valores normais do console).

## Instalação

Veja [`nx/LEIA-ME.txt`](nx/LEIA-ME.txt): copie `switch/` para a raiz do cartão,
IWADs em `sd:/switch/gzdoom/iwads/`, mods em `sd:/switch/gzdoom/pwads/` e abra o
`gzdoom_launcher.nro` pelo Homebrew Menu em modo título (segure R ao abrir um jogo).

## Compilar

Veja [`nx/build/README.md`](nx/build/README.md).

## Código

- `src/common/platform/posix/nx/`: sistema do Switch (threads, crash log, tela de
  carregamento, glthread experimental)
- `tools/nxlauncher/`: launcher
- `src/common/engine/nx_*`: workers, perf.log e estabilizador de FPS
- `src/common/rendering/gles/gles_asynctex.*`, `src/rendering/hwrenderer/hw_nx*`:
  texturas em segundo plano, prefetch e orçamento de luzes

O README original do UZDoom está em [`docs/README-UZDoom.md`](docs/README-UZDoom.md).

## Licença

GPLv3, como o UZDoom (veja `LICENSE`). Fontes do launcher (Bebas Neue, Poppins):
SIL Open Font License.

---

*English:* Nintendo Switch (homebrew) port of UZDoom 5.0.3 with its own launcher.
Engine resources are identical to upstream 5.0.3, so any mod that runs on UZDoom
5.0.3 works; the changes are Switch-specific loading, threading and frame-rate
stability work. Build instructions in `nx/build/README.md`.
