# Open Watcom 2 environment for building WINVBE (sourced by the build scripts).
# Set WATCOM before running, e.g.  export WATCOM=/opt/watcom
: "${WATCOM:=/opt/watcom}"
export WATCOM
export PATH="$WATCOM/binl64:$WATCOM/binl:$WATCOM/binnt:$WATCOM/binw:$PATH"
export INCLUDE="$WATCOM/h:$WATCOM/h/win"
export EDPATH="$WATCOM/eddat"
