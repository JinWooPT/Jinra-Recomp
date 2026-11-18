# Jinra-Recomp

[English](README.en.md) · Português

Compilador local para **The Legend of Zelda: Ocarina of Time** da Nintendo Switch 2, versão 1.1.1 (`e0f3ccfa`), para Windows x64 com Vulkan.

## Primeira compilação

1. Abre `dist/Jinra-Recomp.exe`. O executável contém a aplicação, Python, LLVM/MinGW, as bibliotecas e o código do motor. A primeira abertura extrai a aplicação e a preparação das ferramentas ocorre quando precisa delas.
2. Confirma a cópia selecionada. Podes escolher um arquivo `.nx`, `.nup` ou uma pasta com `exefs` e `romfs` já extraídas. **Verificar cópia** confirma a versão antes de compilar.
3. Escolhe uma pasta de destino vazia. Um SSD com cerca de 60 GB livres dá margem para os ficheiros do jogo e a compilação. Seleciona **Compilar jogo** e acompanha a etapa atual ou abre o **Registo**.
4. Quando a compilação terminar, seleciona **Jogar**. **Medir fluidez e RAM ao jogar** desativa a geração de fotogramas e guarda três CSV por sessão em `diagnostico` na pasta do jogo. Os ficheiros registam os tempos de fotograma, o tempo gasto em operações como preparar shaders e carregar texturas, e o uso de RAM pelo processo a cada cinco segundos. Mantém o launcher aberto durante a medição.
5. **Poupar RAM ao jogar** evita carregar a cache Vulkan do jogo. Este modo pode aumentar as pausas ao encontrar efeitos gráficos novos. A opção começa desativada.

Depois de compilar, abre `Jinra-Ocarina.exe` no Ambiente de Trabalho. Este executável abre o launcher instalado, onde podes escolher **Jogar**, **Atualizações**, **Poupar RAM ao jogar** ou **Medir fluidez e RAM ao jogar**. Podes apagar o ficheiro de setup que descarregaste.

## Atualizações e saves

Abre **Atualizações** no launcher para procurar a versão estável no GitHub. O Jinra valida o tamanho e o SHA-256 do pacote antes de o abrir. Se o motor tiver mudado, recompila na instalação existente e reutiliza a extração e os ficheiros compilados compatíveis. Uma atualização só do launcher mantém o motor instalado. Para instalar a 0.1.8, fecha o jogo, abre **Jinra-Ocarina → Atualizações** e aplica a atualização à pasta onde já jogas. O motor mudou nesta versão. O launcher recompila localmente os módulos necessários e aproveita os objetos compatíveis que ainda estiverem em disco. Os saves e as definições ficam na pasta do jogo.

No emulador, abre a pasta de saves do Ocarina of Time. No launcher, usa **Importar saves** e seleciona a pasta que contém diretamente `slot_00`, `slot_01` e as restantes subpastas de partidas. O Jinra importa todos os slots válidos dessa pasta. Podes selecionar a pasta antes da primeira compilação ou importar numa instalação existente com o jogo fechado. Os slots importados substituem os que têm o mesmo número. A cópia anterior fica em `saves-backup` na pasta do jogo.

Podes escolher **English** ou **Português** no seletor de idioma do setup. O launcher guarda a escolha. O idioma das definições dentro do jogo é escolhido no próprio jogo.

O jogo guarda o registo atual em `logs/jinra_log.txt` e conserva os quatro anteriores na mesma pasta. No launcher, abre **Registo → Logs do jogo**. Durante o jogo, **F1 → Diagnóstico → Guardar relatório de diagnóstico** reúne os registos recentes, as definições, os dados do PC e uma captura em `logs/reports`. A cópia preparada para partilha omite o nome de utilizador e a pasta pessoal.

## NVIDIA DLSS 4.5

Ativa **NVIDIA DLSS 4 / 4.5 e Multi Frame Generation** antes de compilar ou atualizar. Depois de aceitares a licença, o launcher descarrega o SDK 310.9.16 diretamente da NVIDIA e valida os ficheiros. Em **F1 → Imagem**, escolhe **Método de reconstrução → NVIDIA DLSS**, o **Modelo de reconstrução DLSS** e a **Resolução 3D**. O modelo **K** corresponde ao DLSS 4. Os modelos **M/L** correspondem ao DLSS 4.5. Em **Desempenho**, escolhe o **Multiplicador de fotogramas** e o **Motor de geração de fotogramas**. Os multiplicadores NVIDIA disponíveis dependem da placa e do controlador.

## AMD FSR

O FSR 3.1 vem incluído. Para adicionar o FSR 4.1.2, marca **AMD FSR 4 em placas compatíveis** antes de compilar ou atualizar. O launcher valida e instala os componentes oficiais da AMD. Em **F1 → Imagem → Método de reconstrução**, o jogo disponibiliza os métodos que a placa e o controlador suportam. A **Resolução 3D** ajusta a escala usada pelo método escolhido.

Para gerar fotogramas com a AMD, abre **F1 → Desempenho**, seleciona **Motor de geração de fotogramas → AMD FSR 3.1 (2x)** e coloca o **Multiplicador de fotogramas** em **2x**. Podes combinar este motor com a reconstrução FSR 3.1 ou FSR 4. O HUD pode ficar desfocado por instantes em movimento. O overlay indica o motor que gerou os fotogramas recentes.

## Idioma e overlay

Em **F1 → Interface → Idioma do Jinra**, podes escolher **Português (Brasil)** ou **Português (Portugal)** para as definições e o overlay.

**Ctrl+V** mostra ou oculta o overlay. Em **F1 → Interface**, podes escolher o modo e o canto do ecrã. Os modos com gráficos mostram FPS base, FPS apresentados, tempo por fotograma e consumo de RAM e VRAM.

O launcher deteta as threads de CPU disponíveis ao processo e lê a memória física e o limite de memória do Windows. A tradução do código e a compilação escolhem o paralelismo conforme a memória disponível. O compilador volta a verificar a memória durante a compilação, antes de iniciar mais tarefas.

O launcher guarda as ferramentas e o registo de compilação em `%LOCALAPPDATA%\Jinra-Recomp`. A cópia do Zelda é lida localmente. O pacote de instalação contém o código e as ferramentas de compilação. Os dados do jogo, o executável do jogo, os saves e as caches ficam no PC que os gera.

## Componentes

- **Jinra-Recomp** tem uma aplicação Windows, empacotamento, importação NX e fluxo de diagnóstico próprios. O ícone e a paleta vêm do Jinra.
- **Motor.** O código em `engine/` trata da tradução ARM64, dos serviços da consola e da renderização Vulkan. A RomFS fica diretamente no destino da instalação.
- **Entrada NX.** O importador usa o leitor MIT do [NXFormat 1.0.3](https://github.com/fab918/nxformat/releases/tag/V1.0.3). Valida a edição do jogo, seleciona a atualização correspondente, reúne a base e a atualização e verifica a integridade dos ficheiros ao extrair. A RomFS importada fica diretamente no destino, dispensando uma segunda cópia completa.

O código do projeto e a integração do motor seguem GPL-3.0-or-later. Os componentes incorporados conservam os avisos e as licenças junto do respetivo código.

## Empacotar o instalador

`python package.py` recompila o launcher e gera `dist/Jinra-Recomp.exe`. Precisa de Python 3.12 ou mais recente e do .NET SDK 10 com os pacotes Windows x64 de distribuição autónoma. Os ZIP `python.zip`, `cpm.zip`, `llvm.zip`, `app.zip` e `third_party.zip` em `.local` são verificados pelo SHA-256 antes de serem usados. O LLVM está indicado em launcher/tools.json.

Para publicar uma versão, usa uma tag `v` seguida da versão do launcher e anexa `Jinra-Recomp.exe` e `SHA256SUMS.txt` à release estável. Mantém `GameVersion` em `launcher/Builder.cs` quando a alteração for só ao launcher.

## Primeiro teste do jogo

Depois de compilar, confirma a abertura, o menu e a entrada em Kokiri. Para medir pausas, faz o mesmo percurso duas vezes com as mesmas definições. Para medir crescimento de RAM, repete o percurso durante a mesma sessão e compara o consumo quando regressas ao mesmo ponto. Guarda os CSV e o registo do jogo se houver bloqueios, erros gráficos ou quebras de desempenho. A comparação de Hyrule Field e Castle Town fica para depois de confirmar a primeira partida.
