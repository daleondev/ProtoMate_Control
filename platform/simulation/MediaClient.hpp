#pragma once
#include <cstddef>
extern "C" bool protomate_sim_media_info(unsigned device, bool* created);
extern "C" bool protomate_sim_media_read(unsigned device, std::size_t offset, void* data, std::size_t size);
extern "C" bool protomate_sim_media_write(unsigned device,
                                          std::size_t offset,
                                          const void* data,
                                          std::size_t size);
extern "C" bool protomate_sim_media_erase(std::size_t offset, std::size_t size);

extern "C" bool protomate_sim_media_flush(unsigned device);
