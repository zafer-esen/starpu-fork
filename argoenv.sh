#ARGO_PATH="/proj/snic2016-7-69/argodsm"
#ARGO_PATH="/proj/snic2017-7-43/argodsm-stage"
ARGO_PATH="/home/zafer/projargo/argo-magnus/argodsm"
#ARGO_PATH=$ARGO_PATH:"/proj/snic2016-7-69/argodsm-stage/build"
#ARGO_PATH=$ARGO_PATH:"/proj/snic2016-7-69/argodsm-stage/src"
#FXT_PATH="/proj/snic2016-7-69/fxt/build"
#OPENBLAS_PATH="/home/mano9519/OpenBLAS"

LD_LIBRARY_PATH="${ARGO_PATH}/build/lib"
#LD_LIBRARY_PATH="${ARGO_PATH}/build/lib:${FXT_PATH}/lib"

LDFLAGS="-L${ARGO_PATH}/build/lib"
#LDFLAGS="-L${ARGO_PATH}/build/lib -L${FXT_PATH}/lib"

CFLAGS="-I${ARGO_PATH}/src"
#FXT_CFLAGS="-I${FXT_PATH}/include -I${FXT_PATH}"
CPPFLAGS="-I${ARGO_PATH}/src"
#CPPFLAGS="-I${ARGO_PATH}/src -I${FXT_PATH}/src"

LIBS="-largo -largobackend-mpi"
#FXT_LIBS="-L${FXT_PATH}/lib -lfxt"


INCLUDE=-I$CFLAGS:${ARGO_PATH}
export INCLUDE
export LD_LIBRARY_PATH
export LDFLAGS
export CFLAGS
export FXT_CFLAGS
export CPPFLAGS
export LIBS
export FXT_LIBS
