# Makefile for tests - builds unit-test binaries

CFLAGS+=-I../ -I. -g
LDFLAGS+=-lm

SUITE_NAMES = artifact cave command effects game message monster \
	object parse player trivial z-dice z-expression z-file \
	z-quark z-queue z-textblock z-util z-virt

.PHONY : all build run clean clean-coverage list-suites \
	$(SUITE_NAMES:%=build-%) $(SUITE_NAMES:%=run-%)

all : run

# Sorted alphabetically
SUITES = \
	artifact/suite.mk \
	cave/suite.mk \
	command/suite.mk \
	effects/suite.mk \
	game/suite.mk \
	message/suite.mk \
	monster/suite.mk \
	object/suite.mk \
	parse/suite.mk \
	player/suite.mk \
	trivial/suite.mk \
	z-dice/suite.mk \
	z-expression/suite.mk \
	z-file/suite.mk \
	z-quark/suite.mk \
	z-queue/suite.mk \
	z-textblock/suite.mk \
	z-util/suite.mk \
	z-virt/suite.mk

include $(SUITES)

TESTOBJS  := $(TESTPROGS:%=%.o)
# Add an extension so suffix rules can be used.
TESTPROGS := $(TESTPROGS:%=%.exe)

TESTOBJS += test-utils.o unit-test.o

# gcov intermediate data
GCOBJS = $(TESTOBJS:.o=.gcno) $(TESTOBJS:.o=.gcda)

include Makefile.inc

build : $(TESTPROGS)

run : build
	@test x"$CROSS_COMPILE" = xyes || ( \
		if [ -n "$(SUITE)" ]; then \
			./run-tests -s "$(SUITE)"; \
		else \
			./run-tests; \
		fi)

list-suites :
	@echo "Available test suites:"
	@for s in $(SUITE_NAMES); do echo "  $$s"; done

define suite_template
$(1)_PROGS = $$(filter $(1)/%,$$(TESTPROGS))
build-$(1): $$($(1)_PROGS)
run-$(1): build-$(1)
	@test x"$$(CROSS_COMPILE)" = xyes || ./run-tests -s $(1)
endef

$(foreach s,$(SUITE_NAMES),$(eval $(call suite_template,$(s))))

.SUFFIXES : .exe

.c.o :
	@$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(TESTPROGS) : ../angband.a test-utils.o unit-test.o
.o.exe :
	@$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< \
		test-utils.o unit-test.o ../angband.a \
		$(LDFLAGS) $(LDADD) $(LIBS)
	@echo "  CC $@"

clean :
	-$(RM) $(TESTOBJS) $(TESTPROGS)

clean-coverage :
	-$(RM) $(GCOBJS)

.PRECIOUS : %.o
