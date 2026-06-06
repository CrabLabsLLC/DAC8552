cmake_minimum_required(VERSION 3.20)

if(IDF_VERSION_MAJOR)
	idf_component_register(
		SRCS
			"src/dac8552.c"
		INCLUDE_DIRS "include"
	)
elseif(ZEPHYR_BASE)
	zephyr_library()
	zephyr_library_sources(
		src/dac8552.c
	)
	zephyr_include_directories(include)
else()
	add_library(dac8552 STATIC
		src/dac8552.c
	)
	target_include_directories(dac8552 PUBLIC include)
endif()
