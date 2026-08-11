RUN --mount=type=cache,target=/tmp "$USER" '${RAW}' [one,{two}] ${CACHE:-/tmp} $PATH \
    --flag=value "escaped\\n$HOME" 'literal' [three,{four}] ${NEXT} $USER \
    echo ${HOME:-/root} $PATH # continued comment
ENV A=one \
    B=two \
    C=three
