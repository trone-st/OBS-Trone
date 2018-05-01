project(obs-image)

set(obs-image_SOURCES
	obs-image.c)

add_library(obs-image MODULE
	${obs-image_SOURCES})
target_link_libraries(obs-image
	)
install_obs_core(obs-image)

