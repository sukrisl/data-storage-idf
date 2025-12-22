idf_component_register(
  SRCS "mountable.c" "storage_worker.c"
  INCLUDE_DIRS "include"
  REQUIRES littlefs
)