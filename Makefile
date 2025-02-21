EXECUTABLE  = capture_vpe_display

SRCDIR        = src src/utils
INCLUDEDIR    = $(SRCDIR) 
INCLUDEDIR   +=/home/workspace/ti-processor-sdk-linux-am57xx-evm-05.02.00.10/linux-devkit/sysroots/armv7ahf-neon-linux-gnueabi/usr/include/omap 
INCLUDEDIR   +=/home/workspace/ti-processor-sdk-linux-am57xx-evm-05.02.00.10/linux-devkit/sysroots/armv7ahf-neon-linux-gnueabi/usr/include/libdrm
OBJDIR        = obj
DEPDIR        = deps
BINDIR        = bin
EXCLUDE_FILES = src/utils/vpe-common.c src/utils/display-x11.c src/utils/display-kmscube.c src/utils/display-wayland.c src/viddec3test.c
EXCLUDE_FILES+= src/v4l2capturedisplay.c src/videnc2test.c src/utils/demux.h src/utils/demux.c

MY_DEFINE	:=
#MY_DEFINE	+= -DDEBUG_ON_PC_SIDE

LIBRARIES 	:=
LIBRARIES 	+= -ldrm -ldrm_omap

MAKE_DLL 	:=
#MAKE_DLL	+= -shared

CXXFLAGS += "-std=c++14" $(MY_DEFINE) -fPIC -pipe
CPPFLAGS +=$(MY_DEFINE) -fPIC -pipe
CFLAGS	+= $(MY_DEFINE) -pipe
LDFLAGS += $(MAKE_DLL) $(LIBRARIES) -pipe

ifeq ($(BUILD_MODE),debug)
	CFLAGS += -O0
	CFLAGS += -g3
else ifeq ($(BUILD_MODE),run)
	CFLAGS += -O2
else
#	$(error Build mode $(BUILD_MODE) not supported by this Makefile)
	CFLAGS += -O0
	CFLAGS += -g3
endif


all: createdir build_info $(OBJDIR)/$(EXECUTABLE) print_size copy_to_bin videnc2test v4l2capturedisplay #viddec3test


v4l2capturedisplay:
	@make -f Makefile_v4l2capturedisplay	

clean_v4l2capturedisplay:
	@make -f Makefile_v4l2capturedisplay clean

viddec3test:
	@make -f Makefile_viddec3test	

clean_viddec3test:
	@make -f Makefile_viddec3test clean
	
videnc2test:
	@make -f Makefile_videnc2test	

clean_videnc2test:
	@make -f Makefile_videnc2test clean


PROJECT_ROOT = $(dir $(abspath $(lastword $(filter-out deps/%, $(MAKEFILE_LIST)))))

define make_obj_list				
 objects += $(addprefix $(2)/, $(addsuffix .o, $(notdir $(basename \
 $(filter-out $(sort $(EXCLUDE_FILES)), $(sort $(wildcard $(addsuffix /*.c*, $(1)))))))))
 
 objects_c += $(filter-out $(sort $(EXCLUDE_FILES)), $(sort $(wildcard $(addsuffix /*.c, $(1)))))
 
 objects_c_o += $(addprefix $(2)/, $(notdir $(patsubst %.c, %.o, \
 $(filter-out $(sort $(EXCLUDE_FILES)), $(sort $(wildcard $(addsuffix /*.c, $(1))))))))
 
 objects_c_d += $(addprefix $(3)/, $(notdir $(patsubst %.c, %.d, \
 $(filter-out $(sort $(EXCLUDE_FILES)), $(sort $(wildcard $(addsuffix /*.c, $(1))))))))
 
 objects_cpp += $(filter-out $(sort $(EXCLUDE_FILES)), $(sort $(wildcard $(addsuffix /*.cpp, $(1)))))
 
 objects_cpp_o += $(addprefix $(2)/, $(notdir $(patsubst %.cpp, %.o, \
 $(filter-out $(sort $(EXCLUDE_FILES)), $(sort $(wildcard $(addsuffix /*.cpp, $(1))))))))
 
 objects_cpp_d += $(addprefix $(3)/, $(notdir $(patsubst %.cpp, %.d, \
 $(filter-out $(sort $(EXCLUDE_FILES)), $(sort $(wildcard $(addsuffix /*.cpp, $(1))))))))
endef

$(foreach src, $(SRCDIR), $(eval $(call make_obj_list, $(src), $(OBJDIR), $(DEPDIR))))

$(foreach s,$(objects_cpp),$(foreach o,$(filter %$(basename $(notdir $s)).o,$(objects)),$(eval $o: $s)))
$(foreach s,$(objects_c),$(foreach o,$(filter %$(basename $(notdir $s)).o,$(objects)),$(eval $o: $s)))

build_info:
	$(foreach s,$(objects_cpp),$(foreach o,$(filter %$(basename $(notdir $s)).o,$(objects)),$(info New rule: $o: $s)))
	$(foreach s,$(objects_c),$(foreach o,$(filter %$(basename $(notdir $s)).o,$(objects)),$(info New rule: $o: $s)))

INC_PARAMS = $(foreach d, $(INCLUDEDIR), -I$d)
DEPFLAGS   = -MT $@ -MMD -MP -MF $(DEPDIR)/$(notdir $*.d)

$(objects_cpp_o):
	@echo ------------------
	@echo Build *.cpp $@ from $<
	@echo ------------------
	$(CXX) $(DEPFLAGS) $(INC_PARAMS) -c $(CFLAGS) $(CXXFLAGS) -o $@ $<

$(objects_c_o):
	@echo ------------------
	@echo Build *.c $@ from $<
	@echo ------------------
	$(CC) $(DEPFLAGS) $(INC_PARAMS) -c $(CFLAGS) $(CPPFLAGS) -o $@ $<

$(OBJDIR)/$(EXECUTABLE): $(objects)
	@echo ------------------
	@echo Link $@ from $^
	@echo ------------------
	$(CXX) -o $@ $^ $(LDFLAGS)

createdir:
	@mkdir -p $(OBJDIR)
	@mkdir -p $(DEPDIR)
	@mkdir -p $(BINDIR)
	
# list of all directories
dirs = $(patsubst %/, %, $(shell ls -d */))
print_dir:
	@echo ----------------------------------
	@echo list dir is $(dirs)
	@echo ----------------------------------
	@$(foreach dir,$(dirs),echo $(dir);)

print_dir_src:
	@echo ----------------------------------
	@echo list dir is $(INC_PARAMS)
	@echo ----------------------------------
	@$(foreach dir,$(INC_PARAMS),echo $(dir);)

print_dbg: print_dir
	@echo ----------------------------------
	@echo objects		= $(objects)
	@echo objects_c		= $(objects_c)
	@echo objects_cpp	= $(objects_cpp)
	@echo objects_c_o	= $(objects_c_o)
	@echo objects_cpp_o	= $(objects_cpp_o)
	@echo objects_c_d	= $(objects_c_d)
	@echo objects_cpp_d	= $(objects_cpp_d)
	@echo ----------------------------------
	@$(foreach src, $(SRCDIR), echo $(src);)
	@echo ----------------------------------
	
print_size: $(OBJDIR)/$(EXECUTABLE)
	@echo 'Invoking: GNU ARM Cross Print Size'
	 $(TOOLCHAIN_SYS)-size --format=berkeley "$(OBJDIR)/$(EXECUTABLE)"
	@echo 'Finished building: $(OBJDIR)/$(EXECUTABLE)'
	@echo ' '

copy_to_bin: print_size
	@echo -----------------------------------------
	@echo 'copy $(EXECUTABLE) to $(BINDIR) directory';
	@cp $(OBJDIR)/$(EXECUTABLE) $(BINDIR)/$(EXECUTABLE);
	@rm -fr $(OBJDIR)/$(EXECUTABLE);
	@echo -----------------------------------------
	@echo ' '
	
PATH_TO_SDK := $(shell v='$(SDK_PATH_TARGET)'; echo "$${v%*}")
	
clean: clean_viddec3test clean_videnc2test clean_v4l2capturedisplay
	rm -fr $(OBJDIR) $(DEPDIR) $(BINDIR)
	
.PHONY: copy_to_bin build_info clean all

-include $(objects_c_d)
-include $(objects_cpp_d)
