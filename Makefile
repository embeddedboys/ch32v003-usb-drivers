
PROJECTS:=bootloader \
	examples/demo_composite_hid \
	examples/cdc_exp \
	vendor

all : build

build :
	for dir in $(PROJECTS); do make -C $$dir build; done

clean : $(PROJECTS)
	for dir in $(PROJECTS); do make -C $$dir clean; done

.PHONY : $(PROJECTS)
