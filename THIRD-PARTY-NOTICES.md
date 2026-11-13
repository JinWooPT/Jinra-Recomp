# Avisos de terceiros

O Jinra-Recomp modifica e integra código do motor [Hookshot 2.2.0-rc.1](https://github.com/NotRexed/Hookshot/releases/tag/v2.2.0-rc.1), commit `4aee23659b9436dab2cd7be1ac07333c06246b7c`, sob a GNU GPL v3. As alterações do Jinra até 13 de novembro de 2026 incluem a aplicação Windows, a identidade do jogo, importação NX e de saves, atualização incremental, diagnósticos e integração das opções gráficas. Os autores e os avisos originais de cada componente mantêm-se aplicáveis.

As alterações ao tradutor gráfico incluem a correção do `Sqrt` de AlfaDev23 e a correção de campos de bits publicadas no [Ocarina Recomp #91](https://github.com/sostenesfreitas/ocarina-recomp/pull/91) e [#203](https://github.com/sostenesfreitas/ocarina-recomp/pull/203). O [NX2navi](https://github.com/tangerineturtle1/NX2navi) e o trabalho de Ivan0xFF em `recomp/patches-v111.txt` contribuíram com correções e referências para a renderização e ecrã ultralargo. Aplicam-se os avisos e as licenças das respetivas contribuições.

Os avisos de componentes incorporados, incluindo A64, Eden, Sirit, Dear ImGui, SDL, Zstandard, AMD FidelityFX, Opus, fontes e NXFormat, encontram-se junto do respetivo código. Os SDK opcionais NVIDIA e AMD têm as suas próprias licenças.

## Entradas de empacotamento

Coloca os componentes `python`, `cpm`, `app` e `third_party` da [lista de componentes da versão original](https://github.com/NotRexed/Hookshot/releases/download/v2.2.0-rc.1/hookshot-setup.txt) em `.local/`, com os nomes `.zip` esperados pelo empacotador. O LLVM é a [versão LLVM-MinGW UCRT x86-64 20261104](https://github.com/mstorsjo/llvm-mingw/releases/tag/20261104). Os hashes esperados estão em `launcher/tools.json` e `package.py`.
