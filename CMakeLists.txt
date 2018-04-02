cmake_minimum_required( VERSION 2.8.8 )

# The "MSVC" flag isn't set until the "project" command
# is called.  Let's just check the operating system.
if( NOT WIN32 )
	project( multimon-ng C )
else( NOT WIN32 )
	# Visual Studio C compiler doesn't support C99 (i.e. stdbool.h);
	# so use the Visual Studio C++ compiler instead
	project( multimon-ng CXX )
endif( NOT WIN32 )

set( TARGET "${PROJECT_NAME}" )
set( VERSION "1.1.5" )
set( MAJOR "1" )
set( MINOR "1" )
set( PATCH "5" )
set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra" )

if( WIN32 )
	add_definitions( "-DWIN32_AUDIO" "-DONLY_RAW" "-DWINDOWS" )
	link_libraries( ${TARGET} "-lwinmm" )
	set( SOURCES ${SOURCES}
		"win32_soundin.c"
		)
elseif( UNIX )
	find_package( X11 )
	if ( X11_FOUND )
		option( X11_SUPPORT "Enable X11 display support" ${X11_FOUND} )
		mark_as_advanced( X11_SUPPORT )
	endif( X11_FOUND )
	find_package( PulseAudio )
	if ( PULSEAUDIO_FOUND )
		option( PULSE_AUDIO_SUPPORT "Enable pulse audio support" ${PULSEAUDIO_FOUND} )
		mark_as_advanced( PULSE_AUDIO_SUPPORT )
	endif( PULSEAUDIO_FOUND )

	# Check if we can use the GCC/llvm __builtin_popcount
	include( CheckCSourceCompiles )
	check_c_source_compiles(
		"int main() { __builtin_popcount(42); return 0; }" USE_BUILTIN_POPCOUNT )

endif( WIN32 )

if( X11_SUPPORT )
	include_directories( ${X11_INCLUDE_DIR} )
	link_libraries( ${X11_LIBRARIES} )
	set( SOURCES ${SOURCES}
		xdisplay.c
		demod_display.c
	)
else( X11_SUPPORT )
	add_definitions( "-DNO_X11" )
endif( X11_SUPPORT ) 

if( PULSE_AUDIO_SUPPORT )
	include_directories( ${PULSEAUDIO_INCLUDE_DIR} )
	find_library( PULSE_SIMPLE NAMES pulse-simple REQUIRED )
	link_libraries( ${PULSEAUDIO_LIBRARY} ${PULSE_SIMPLE} )
	add_definitions( "-DPULSE_AUDIO" )
else( PULSE_AUDIO_SUPPORT )
	add_definitions( "-DDUMMY_AUDIO" )
endif( PULSE_AUDIO_SUPPORT )

if( NOT MSVC )
	add_definitions( "-std=gnu99" )
endif( NOT MSVC )
add_definitions( "-DMAX_VERBOSE_LEVEL=3" "-DCHARSET_UTF8" )

set( HEADERS ${HEADERS}
    	multimon.h
    	gen.h
    	filter.h
    	filter-i386.h
	)

set( SOURCES ${SOURCES}
	unixinput.c
	uart.c
	pocsag.c
	selcall.c
	hdlc.c
	demod_zvei1.c
	demod_zvei2.c
	demod_zvei3.c
	demod_pzvei.c
	demod_dzvei.c
	demod_ccir.c
	demod_eia.c
	demod_eea.c
	demod_ufsk12.c
	demod_poc24.c
	demod_poc12.c
	demod_poc5.c
	demod_hapn48.c
	demod_fsk96.c
	demod_dtmf.c
	demod_clipfsk.c
	demod_fmsfsk.c
	demod_afsk24.c
	demod_afsk24_3.c
	demod_afsk24_2.c
	demod_afsk12.c
	demod_flex.c
	BCHCode.c
	costabi.c
	costabf.c
	clip.c
	fms.c
	demod_eas.c
	demod_morse.c
	demod_dumpcsv.c
	demod_x10.c
	)


add_executable( "${TARGET}" ${SOURCES} ${HEADERS} )
set_property(TARGET "${TARGET}" PROPERTY LINKER_LANGUAGE C)
target_link_libraries( "${TARGET}" m )
install(TARGETS multimon-ng DESTINATION bin)

