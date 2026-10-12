# Source membership and exact Unity groups come from the Bazel production
# inventory generated at configure time.
seekdb_apply_unity_inventory(ob_share SEEKDB_SHARE_UNITY)
seekdb_apply_standalone_inventory(ob_share SEEKDB_SHARE_STANDALONE)
seekdb_apply_standalone_inventory(ob_share_extra SEEKDB_SHARE_DATUM_STANDALONE)

ob_add_new_object_target(ob_share ob_share)
ob_add_new_object_target(ob_share_extra ob_share_extra)
target_link_libraries(ob_share PUBLIC ob_base config)
target_link_libraries(ob_share_extra PUBLIC ob_base_without_pass)
target_sources(ob_share PRIVATE geo/srs_metadata_adapter.cpp)

if (SEEKDB_ENABLE_EXPERIMENTAL_PLUGINS)
  target_sources(ob_share PRIVATE
    schema/routine_catalog_transaction.cpp
    plugin/extension_package.cpp
    plugin/ob_plugin_catalog.cpp
    plugin/ob_plugin_loader.cpp
    plugin/ob_plugin_registry.cpp
    plugin/ob_plugin_sql_catalog.cpp)
  target_link_libraries(ob_share PUBLIC seekdb_plugin_sdk seekdb_rust_host)
endif()

# SQL adapters use ABI shims; geometry parsing and SRS/codec work belongs to
# the plugin. Unimplemented shims return OB_NOT_SUPPORTED.
target_sources(ob_share PRIVATE geo/ob_geo_plugin_stubs.cpp)
# Storage MBR transport and leased SPI invocation only.
target_sources(ob_share PRIVATE geo/spatial_index_plugin.cpp)
target_sources(ob_share PRIVATE geo/srs_plugin.cpp)

add_library(ob_share_static STATIC EXCLUDE_FROM_ALL)
target_link_libraries(ob_share_static PUBLIC ob_share ob_share_extra)
