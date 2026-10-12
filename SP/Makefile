#
# iowolfsp Makefile
#
# GNU Make required
#
COMPILE_PLATFORM=$(shell uname | sed -e 's/_.*//' | tr '[:upper:]' '[:lower:]' | sed -e 's/\//_/g')
COMPILE_ARCH=$(shell uname -m | sed -e 's/i.86/x86/')

ifeq ($(COMPILE_PLATFORM),sunos)
  # Solaris uname and GNU uname differ
  COMPILE_ARCH=$(shell uname -p | sed -e 's/i.86/x86/')
endif

ifndef BUILD_STANDALONE
  BUILD_STANDALONE =
endif
ifndef BUILD_CLIENT
  BUILD_CLIENT     =
endif
ifndef BUILD_SERVER
  BUILD_SERVER     = 0
endif
ifndef BUILD_GAME_SO
  BUILD_GAME_SO    =
endif
ifndef BUILD_GAME_QVM
  BUILD_GAME_QVM   = 0
endif
ifndef BUILD_BASEGAME
  BUILD_BASEGAME =
endif
ifndef BUILD_RENDERER_REND2
  BUILD_RENDERER_REND2 =
endif

#############################################################################
#
# If you require a different configuration from the defaults below, create a
# new file named "Makefile.local" in the same directory as this file and define
# your parameters there. This allows you to change configuration without
# causing problems with keeping up to date with the repository.
#
#############################################################################
-include Makefile.local

ifeq ($(COMPILE_PLATFORM),cygwin)
  PLATFORM=mingw32
endif

ifndef PLATFORM
PLATFORM=$(COMPILE_PLATFORM)
endif
export PLATFORM

# ... truncated for size test - FULL CONTENT COMING NEXT
