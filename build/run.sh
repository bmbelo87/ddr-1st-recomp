#!/usr/bin/env bash
# Roda o DDR recompilado. Por padrao SEM instrumentacao (estado de jogo).
#   ./run.sh                 -> jogo limpo, so as correcoes reais
#   FIX=1 ./run.sh           -> + contencao do travamento (corta o ciclo)
#   DANCER=0 ./run.sh        -> comeca com o dancarino 3D desligado (F5 alterna)
#   CHECK=1 ./run.sh         -> + diagnostico (detector de ciclo / validador)
#   GTE=700 ./run.sh         -> + probe de projecao do GTE (implica CHECK)
#   NOMENU=1 ./run.sh        -> sem o hook de menu (nada de EXIT GAME)
#   NOEXIT=1 ./run.sh        -> mantem o menu de 7 itens, sem o EXIT GAME
#   EXITX=-36 EXITY=165 ./run.sh -> move o texto EXIT GAME
#   EXITTEXT="QUIT" ./run.sh -> troca o texto
#   RAWSTR=0x80017D10 ./run.sh -> usa uma string que ja existe na imagem
#   SWEEP=1 ./run.sh         -> EXIT A..H nos Y dos itens, para alinhar
#   HDRTEST=1 ./run.sh       -> escreve EXIT GAME NO LUGAR do cabecalho (diagnostico)
#   TEXTTEST=1 ./run.sh      -> sequestra o cabecalho, na posicao do EXIT
#   ITEMS=01234 ./run.sh     -> volta ao menu original de 5 itens
#   EXITITEM=6 ./run.sh      -> faz o item 6 encerrar o jogo (teste)
#   MENUY0=60 MENUSTEP=15 ./run.sh -> ajusta origem/espacamento da lista
#   UNLOCK=1 ./run.sh        -> acende EDIT e as musicas escondidas (nao salva)
#   UNLOCK=0x10 ./run.sh     -> so o bit do EDIT
#   PRIM=1 ./run.sh          -> vigia o buffer de primitivas
#   OTP=1 ./run.sh           -> sonda do balde da OT
#   TRACE=1 ./run.sh         -> detector do ciclo + rastreio de escrita na OT do modelo
#   CHAIN=1 ./run.sh         -> checa a lista de blocos do modelo (repeticao)
#   NOGUARD=1 ./run.sh       -> desliga a guarda do ponteiro de animacao
#   NODEDUP=1 ./run.sh       -> desliga a supressao do desenho duplicado
#   NOBUILD=1 ./run.sh       -> nao recompila antes
set -u
LABEL="${1:-run}"
mkdir -p logs
LOG="logs/${LABEL}-$(date +%m%d-%H%M%S).log"

# O toggle F5 precisa saber o endereco do emissor; nao custa nada em execucao.
export PSX_DANCER_FUNC=0x80022268
export PSX_MENU_EXIT=0x8004A380   # navegacao/desenho do EXIT GAME
export PSX_MENU_DRAW=0x80049F9C   # desenhador da lista (tira o realce)
export PSX_ANIM_GUARD=1           # barra a troca de animacao com ponteiro invalido
export PSX_CHAIN=1                # detecta E suprime o desenho duplicado
[ -n "${DANCER:-}" ] && export PSX_DANCER="$DANCER"
[ -n "${NOMENU:-}"  ] && { unset PSX_MENU_EXIT; unset PSX_MENU_DRAW; }
[ -n "${EXITITEM:-}" ] && export PSX_MENU_EXIT_ITEM="$EXITITEM"
[ -n "${ITEMS:-}"   ] && export PSX_MENU_ITEMS="$ITEMS"
[ -n "${MENUY0:-}"  ] && export PSX_MENU_Y0="$MENUY0"
[ -n "${MENUSTEP:-}" ] && export PSX_MENU_YSTEP="$MENUSTEP"
[ -n "${NOEXIT:-}"  ] && export PSX_MENU_NO_EXIT=1
[ -n "${EXITX:-}"   ] && export PSX_MENU_EXIT_X="$EXITX"
[ -n "${EXITY:-}"   ] && export PSX_MENU_EXIT_Y="$EXITY"
[ -n "${EXITTEXT:-}" ] && export PSX_MENU_EXIT_TEXT="$EXITTEXT"
[ -n "${RAWSTR:-}"  ] && export PSX_MENU_EXIT_STR_RAW="$RAWSTR"
[ -n "${SWEEP:-}"   ] && { export PSX_MENU_EXIT_SWEEP=1; export PSX_MENU_EXIT_Y="${EXITY:--60}"; }
[ -n "${TEXTTEST:-}" ] && export PSX_MENU_TEXT_TEST=1
[ -n "${HDRTEST:-}" ] && { export PSX_MENU_TEXT_TEST=1 PSX_MENU_EXIT_X=-68 PSX_MENU_EXIT_Y=32; }
[ -n "${UNLOCK:-}" ] && export PSX_UNLOCK="$UNLOCK"
[ -n "${PRIM:-}"   ] && export PSX_PRIM_WATCH=1
[ -n "${OTP:-}"    ] && export PSX_OT_PROBE=1
[ -n "${NOGUARD:-}" ] && unset PSX_ANIM_GUARD
[ -n "${NODEDUP:-}" ] && export PSX_NO_DEDUP=1
[ -n "${CHAIN:-}"  ] && export PSX_CHAIN=1 PSX_OTMERGE_CHECK=0x8007259C \
                               PSX_OTMERGE_TRACE=0x8008D2FC:0x8008D324 \
                               PSX_OTMERGE_TRACE2=0x800A3EA0:0x800A3EC0
[ -n "${TRACE:-}"  ] && export PSX_OTMERGE_CHECK=0x8007259C \
                               PSX_OTMERGE_TRACE=0x800845F0:0x800849F0
[ -n "${FIX:-}"    ] && export PSX_OTMERGE_CHECK=0x8007259C PSX_OTMERGE_FIX=1
[ -n "${CHECK:-}"  ] && export PSX_OTMERGE_CHECK=0x8007259C PSX_MODEL_CHECK=0x80022268
[ -n "${GTE:-}"    ] && export PSX_GTE_PROBE="$GTE" PSX_OTMERGE_CHECK=0x8007259C

# O cmake nem sempre esta no PATH (o terminal MSYS puro nao tem o do MINGW64).
# O proprio CMakeCache.txt guarda o caminho absoluto que gerou este build.
find_cmake() {
  [ -n "${CMAKE:-}" ] && { printf %s "$CMAKE"; return; }
  command -v cmake 2>/dev/null && return
  sed -n 's|^CMAKE_COMMAND:INTERNAL=||p' CMakeCache.txt 2>/dev/null |
    sed 's|^\([A-Za-z]\):|/\L\1|' | head -1
}

if [ -z "${NOBUILD:-}" ]; then
  CMAKE="$(find_cmake)"
  if [ -z "$CMAKE" ] || [ ! -x "$CMAKE" ]; then
    echo "!!! cmake nao encontrado. Abra o terminal MINGW64, ou rode com"
    echo "    CMAKE=/c/Program\ Files/CMake/bin/cmake.exe ./run.sh"
    exit 1
  fi
  echo "=== compilando... ($CMAKE)"
  if ! "$CMAKE" --build . --config Release -j >/dev/null 2>build_err.txt; then
    echo "!!! BUILD FALHOU - o executavel NAO foi atualizado:"; tail -25 build_err.txt; exit 1
  fi
  echo "=== build ok"
fi

echo "=== log: $LOG"
./Dance_Dance_Revolution_1st_Mix_Recompiled.exe 2>&1 | tee "$LOG"
echo "=== fim: $LOG"
