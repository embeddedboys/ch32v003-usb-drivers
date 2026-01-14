
PROJECTS:=bootloader \
	demo_composite_hid \
	cdc_exp \
	vendor

all : build

build :
	for dir in $(PROJECTS); do make -C $$dir build; done

clean : $(PROJECTS)
	for dir in $(PROJECTS); do make -C $$dir clean; done

.PHONY : $(PROJECTS)
