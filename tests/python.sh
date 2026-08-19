# shellcheck shell=sh
# Locates a Python interpreter.  Sourced, not run:
#
#   . tests/python.sh
#
# On return PYTHON names the interpreter to use, or is empty if none was
# found; the caller decides whether that is fatal.  A PYTHON already set
# in the environment is honoured as-is and nothing is searched.
#
# Set NEED_NATIVE_PYTHON=1 before sourcing to reject an MSYS2 python.
# It runs perfectly well, but wsgidav depends on bcrypt, which has no
# mingw wheel and needs a Rust toolchain to build from source.  Anything
# that only runs a stdlib script does not need this.
#
# Must be sourced after the caller has cd'd to the top of the tree, so
# that the wsgidav virtualenv is found by a relative path.

# Returns success if $1 is an interpreter this script can use.
#
# Running it is the only reliable test.  On Windows "python3" is usually
# a Microsoft Store stub that exists, resolves, and then refuses to run,
# and "py" is a launcher that is present even when no interpreter is
# registered with it.
#
# sysconfig.get_platform() reports mingw_x86_64_ucrt_gnu under MSYS2
# against win-amd64 for a native Python, which is how the two are told
# apart.
litmus_usable_python() {
    _plat=`"$1" -c "import sysconfig; print(sysconfig.get_platform())" 2>/dev/null` \
        || return 1
    case $_plat in
        "")     return 1 ;;
        mingw*) [ "${NEED_NATIVE_PYTHON-0}" != "1" ] ;;
        *)      return 0 ;;
    esac
}

if [ -z "${PYTHON-}" ]; then
    # A virtualenv left by a previous tests/wsgidav.sh run is a known
    # good interpreter, and already a native one.
    for _candidate in dav-venv/Scripts/python.exe dav-venv/bin/python \
                      python3 python py; do
        if litmus_usable_python "$_candidate"; then
            PYTHON=$_candidate
            break
        fi
    done
fi

# An MSYS2 shell started from the Start menu has a minimal PATH that
# leaves out the Windows one, so a usable Python can be sitting in the
# normal place and still not be found above.  Look there before giving
# up.
if [ -z "${PYTHON-}" ] && command -v cygpath >/dev/null 2>&1; then
    for _base in "${LOCALAPPDATA-}\\Programs\\Python" "${PROGRAMFILES-}" "C:\\"; do
        [ -n "$_base" ] || continue
        _dir=`cygpath -u "$_base" 2>/dev/null` || continue
        [ -d "$_dir" ] || continue
        for _candidate in "$_dir"/Python3*/python.exe "$_dir"/Python*/python.exe; do
            if [ -x "$_candidate" ] && litmus_usable_python "$_candidate"; then
                PYTHON=$_candidate
                echo "-- Using $_candidate --"
                break 2
            fi
        done
    done
fi
