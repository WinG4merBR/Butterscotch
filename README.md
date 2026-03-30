<h1 align="center">Butterscotch</h1>

<p align="center">
<a href="https://discord.gg/2gQR7t3WJR"><img src="https://img.shields.io/discord/1406856655920168971?color=5865F2&logo=discord&logoColor=white&label=discord"></a>
</p>

> [!IMPORTANT]
> Butterscotch is still in a very early stage of development. It is experimental, incomplete, and not yet suitable for general use (but can run Undertale).

Butterscotch is an experimental GameMaker: Studio runner based on the original project created by [MrPowerGamerBR](https://mrpowergamerbr.com), which targeted GLFW and PlayStation®2.

This fork is focused exclusively on the PlayStation®3 and aims to adapt the original work to the PS3 environment, including its graphics stack, toolchain, and platform-specific limitations.

While the original Butterscotch can run multiple games, including titles such as **DELTARUNE (SURVEY_PROGRAM)**, this PlayStation®3-focused version is currently being developed primarily with **UNDERTALE** as its main target.

## How to build
You need [ScummVM Dockerized PS3 Toolchains](https://hub.docker.com/layers/scummvm/dockerized-toolchains/ps3/images/sha256-0b89f032c11e454d1613ba8ac5bfb7f36d47ad61e3d4fb798e7639aed0f21e33) and [PS3GL](https://github.com/Fancy2209/PS3GL) before all.

To build a pkg, run:
```bash
make pkg TITLE=YOUR-GAME_TITLE APPID=YOUR-APP-ID
# e.g make pkg TITLE=UNDERTALE APPID=UNDT00001
```

To build a SELF or ELF, run:
```bash
make self/elf
```

## Tools used

This project was built using:
- PSL1GHT
- PS3GL

## Credits

**MrPowerGamerBR**  
Original creator of Butterscotch.

**Fancy2209**  
Major contributor and creator of PS3GL.