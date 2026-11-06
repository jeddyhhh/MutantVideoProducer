# Runs the monkey test (random commands, clicks, keys, and moves inside every dialog it opens) on a copy of an
# example project: cmake -DEXE=<app> -DSRC=<source dir> -DOUT=<folder> -DSEED=<n> -DSTEPS=<n> -P tests/monkey.cmake
# It fails on a crash, a hang, a broken timeline or a window on the screen; the log is <OUT>/monkey-<SEED>/monkey.txt.
# -DPROJ=<example name> picks another example project (default titles-and-generators).
if(NOT PROJ)
    set(PROJ titles-and-generators)
endif()
set(_dir "${OUT}/monkey-${SEED}")
file(REMOVE_RECURSE "${_dir}")
file(MAKE_DIRECTORY "${_dir}")
file(COPY "${SRC}/data/examples/${PROJ}.mvp" "${SRC}/data/examples/media" DESTINATION "${_dir}")
set(_log "${_dir}/monkey.txt")
execute_process(COMMAND "${EXE}" --monkey-test "${_log}" --steps ${STEPS} --seed ${SEED} "${_dir}/${PROJ}.mvp"
                RESULT_VARIABLE _code TIMEOUT 1500)
if(EXISTS "${_log}")
    file(STRINGS "${_log}" _lines)
    list(LENGTH _lines _n)
    math(EXPR _from "${_n} - 25")
    if(_from LESS 0)
        set(_from 0)
    endif()
    list(SUBLIST _lines ${_from} 25 _tail)
    list(JOIN _tail "\n" _tail)
    message("${_tail}")
endif()
if(NOT _code EQUAL 0)
    message(FATAL_ERROR "monkey seed ${SEED}: exit ${_code} (the log above; replay with --seed ${SEED})")
endif()
