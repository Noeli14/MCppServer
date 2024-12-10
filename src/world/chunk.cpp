#include "chunk.h"

#include <bitset>
#include <iostream>
#include <tag_array.h>
#include <tag_list.h>
#include <tag_string.h>
#include <unordered_set>
#include <nlohmann/json.hpp>

#include "core/config.h"
#include "networking/network.h"
#include "entities/player.h"
#include "region_file.h"
#include "core/server.h"
#include "core/utils.h"
#include "tag_primitive.h"

uint8_t Palette::getIndex(int32_t blockStateID) {
    // Handle new blockStateID
    uint8_t newIndex = static_cast<uint8_t>(indexToBlockState.size());
    blockStateToIndex[blockStateID] = newIndex;
    indexToBlockState.push_back(blockStateID);
    return newIndex;
}

uint8_t MemChunkSection::getOrAddBlockIndex(int32_t blockStateID) {
    auto it = std::ranges::find(palette.indexToBlockState, blockStateID);
    if (it != palette.indexToBlockState.end()) {
        return std::distance(palette.indexToBlockState.begin(), it);
    }
    palette.indexToBlockState.push_back(blockStateID);
    return palette.indexToBlockState.size() - 1;
}

void MemChunkSection::setBlockIndex(int32_t index, uint8_t paletteIndex) {
    int bitOffset = index * bitsPerEntry;
    int byteIndex = bitOffset / 8;
    int bitStart = bitOffset % 8;

    // Ensure blockIndices has enough bytes
    size_t totalBits = static_cast<size_t>(index + 1) * bitsPerEntry;
    size_t totalBytes = (totalBits + 7) / 8;
    if (blockIndices.size() < totalBytes) {
        blockIndices.resize(totalBytes, 0);
    }

    // Clear existing bits
    for (int i = 0; i < bitsPerEntry; ++i) {
        blockIndices[byteIndex + (bitStart + i) / 8] &= ~(1 << ((bitStart + i) % 8));
    }

    // Set new bits
    for (int i = 0; i < bitsPerEntry; ++i) {
        if (paletteIndex & (1 << i)) {
            blockIndices[byteIndex + (bitStart + i) / 8] |= (1 << ((bitStart + i) % 8));
        }
    }

    isEmpty = false;
}

uint8_t MemChunkSection::getBlockIndex(int32_t index) const {
    int bitOffset = index * bitsPerEntry;
    int byteIndex = bitOffset / 8;
    int bitStart = bitOffset % 8;

    uint8_t value = 0;
    for (int i = 0; i < bitsPerEntry; ++i) {
        if (byteIndex + (bitStart + i) / 8 >= blockIndices.size()) {
            return blocks["air"].defaultState; // Default to air if out of bounds
        }
        uint8_t bit = (blockIndices[byteIndex + (bitStart + i) / 8] >> ((bitStart + i) % 8)) & 1;
        value |= (bit << i);
    }

    return value;
}

void MemChunkSection::addBlock(int32_t blockStateID) {
    if (isEmpty && blockStateID != blocks["air"].defaultState) {
        isEmpty = false;
    }
    if (!isEmpty) {
        uint8_t index = palette.getIndex(blockStateID);
        tempBlockIndices.push_back(index);
    }
}

void MemChunkSection::addBiome(int32_t biomeID) {
    uint8_t index = biomePalette.getIndex(biomeID);
    tempBiomeIndices.push_back(index);
}

void MemChunkSection::finalize() {
    if (!isEmpty) {
        bitsPerEntry = calculateBitsPerEntry(palette);
        blockIndices = encodeBlockIndices(tempBlockIndices, bitsPerEntry);
        tempBlockIndices.clear();
    }

    // Finalize biome indices
    if (!biomePalette.indexToBlockState.empty()) {
        int biomeBitsPerEntry = calculateBitsPerEntry(biomePalette);
        biomeIndices = encodeBlockIndices(tempBiomeIndices, biomeBitsPerEntry);
        tempBiomeIndices.clear();
    }
}

Block Chunk::getBlock(int32_t x, int32_t y, int32_t z) const {
    // Validate coordinates
    if (x < 0 || x >= CHUNK_WIDTH || z < 0 || z >= CHUNK_LENGTH || y < MIN_Y || y >= CHUNK_HEIGHT + MIN_Y) {
        throw std::out_of_range("Block coordinates out of range");
    }

    int adjustedY = y - MIN_Y;
    int sectionIndex = adjustedY / SECTION_HEIGHT;
    int localY = adjustedY % SECTION_HEIGHT;

    if (sectionIndex < 0 || sectionIndex >= NUM_SECTIONS) {
        throw std::out_of_range("Y coordinate out of range");
    }

    const auto& sectionOpt = sections[sectionIndex];
    if (!sectionOpt.has_value() || sectionOpt->isEmpty) {
        return Block{blocks["air"].defaultState};
    }

    const MemChunkSection& section = sectionOpt.value();

    // Calculate block position within the section
    int index = (localY * CHUNK_WIDTH * CHUNK_LENGTH) + (z * CHUNK_WIDTH) + x;

    // Retrieve the palette index
    uint8_t paletteIndex = section.getBlockIndex(index);

    // Retrieve the blockStateID from the palette
    if (paletteIndex >= section.palette.indexToBlockState.size()) {
        return Block{blocks["air"].defaultState};
    }

    return Block{section.palette.indexToBlockState[paletteIndex]};
}

void Chunk::setBlock(int32_t x, int32_t y, int32_t z, int32_t blockStateID, bool adjustY) {
    int adjustedY = y;
    if (adjustY) {
        adjustedY = y - MIN_Y;
    }
    // Validate coordinates
    if (adjustY) {
        if (x < 0 || x >= CHUNK_WIDTH || z < 0 || z >= CHUNK_LENGTH || adjustedY < 0 || adjustedY >= CHUNK_HEIGHT) {
            throw std::out_of_range("Block coordinates out of range");
        }
    } else {
        if (x < 0 || x >= CHUNK_WIDTH || z < 0 || z >= CHUNK_LENGTH || adjustedY < MIN_Y || adjustedY >= CHUNK_HEIGHT + MIN_Y) {
            throw std::out_of_range("Block coordinates out of range");
        }
    }

    int sectionIndex = adjustedY / SECTION_HEIGHT;
    int localY = adjustedY % SECTION_HEIGHT;

    if (sectionIndex < 0 || sectionIndex >= NUM_SECTIONS) {
        throw std::out_of_range("Y coordinate out of range");
    }

    auto& sectionOpt = sections[sectionIndex];
    if (!sectionOpt.has_value()) {
        if (blockStateID == blocks["air"].defaultState) return; // No need to store air
        sections[sectionIndex].emplace();
    }

    MemChunkSection& section = sections[sectionIndex].value();

    // Add or retrieve the palette index for the new blockStateID
    uint8_t paletteIndex = section.getOrAddBlockIndex(blockStateID);

    // If adding a new block increases the palette size beyond current bitsPerEntry, update bitsPerEntry and re-encode blockIndices
    int requiredBits = calculateBitsPerEntry(section.palette);
    if (requiredBits > section.bitsPerEntry) {
        // Re-encode all existing block indices with the new bitsPerEntry
        // This involves unpacking the existing blockIndices and re-packing them with the updated bitsPerEntry
        std::vector<uint8_t> currentIndices;
        currentIndices.reserve(CHUNK_WIDTH * CHUNK_LENGTH * SECTION_HEIGHT);

        // Unpack existing indices
        for (int i = 0; i < CHUNK_WIDTH * CHUNK_LENGTH * SECTION_HEIGHT; ++i) {
            currentIndices.push_back(section.getBlockIndex(i));
        }
        // Update bitsPerEntry
        section.bitsPerEntry = requiredBits;

        // Re-pack with new bitsPerEntry
        section.blockIndices = encodeBlockIndices(currentIndices, section.bitsPerEntry);
    }

    // Set the new palette index in blockIndices
    int index = (localY * CHUNK_WIDTH * CHUNK_LENGTH) + (z * CHUNK_WIDTH) + x;
    section.setBlockIndex(index, paletteIndex);

    // Mark the chunk as dirty for future serialization
    markDirty();
}

void Chunk::markDirty() {
    dirty = true;
}

int32_t getLocalCoordinate(int32_t coord) {
    int32_t local = coord % CHUNK_WIDTH;
    if (local < 0) local += CHUNK_WIDTH;
    return local;
}

int calculateBitsPerEntry(const Palette& palette, int min) {
    return std::max(static_cast<int>(std::ceil(std::log2(palette.indexToBlockState.size()))), min); // Minimum 4 bits
}

std::vector<uint8_t> encodePalettedData(const std::vector<uint32_t>& data, int bitsPerEntry, size_t& size) {
    std::vector<uint64_t> longs;
    uint64_t currentLong = 0;
    int bitsFilled = 0;

    for (size_t i = 0; i < data.size(); ++i) {
        const auto& entry = data[i];
        if (bitsPerEntry == 0) {
            // Single-valued palette; no data array needed
            break;
        }

        if (bitsFilled + bitsPerEntry > 64) {
            // Current long is full; push and reset
            longs.push_back(currentLong);
            currentLong = 0;
            bitsFilled = 0;
        }

        // Insert entry into current long
        currentLong |= (static_cast<uint64_t>(entry) << bitsFilled);
        bitsFilled += bitsPerEntry;
    }

    if (bitsFilled > 0) {
        longs.push_back(currentLong);
    }

    size = longs.size();

    // Convert longs to big endian byte order (Minecraft expects big endian for bit-packed data)
    std::vector<uint8_t> encodedData;
    encodedData.reserve(longs.size() * 8);
    for (const auto& l : longs) {
        for (int i = 7; i >= 0; --i) { // Big endian
            encodedData.push_back(static_cast<uint8_t>((l >> (i * 8)) & 0xFF));
        }
    }

    return encodedData;
}

std::vector<uint8_t> encodeBlockIndices(const std::vector<uint8_t>& indices, int bitsPerEntry) {
    std::vector<uint8_t> encoded;
    uint64_t buffer = 0;
    int bitsFilled = 0;

    for (auto index : indices) {
        buffer |= (static_cast<uint64_t>(index) << bitsFilled);
        bitsFilled += bitsPerEntry;

        while (bitsFilled >= 8) {
            encoded.push_back(static_cast<uint8_t>(buffer & 0xFF));
            buffer >>= 8;
            bitsFilled -= 8;
        }
    }

    if (bitsFilled > 0) {
        encoded.push_back(static_cast<uint8_t>(buffer & 0xFF));
    }

    return encoded;
}

std::vector<ChunkCoordinates> getChunksInView(int32_t centerChunkX, int32_t centerChunkZ, int viewDistance) {
    std::vector<ChunkCoordinates> chunks;
    chunks.reserve(static_cast<int64_t>(2 * viewDistance + 1) * (2 * viewDistance + 1));

    for (int dx = -viewDistance; dx <= viewDistance; ++dx) {
        for (int dz = -viewDistance; dz <= viewDistance; ++dz) {
            ChunkCoordinates coords{centerChunkX + dx, centerChunkZ + dz};
            chunks.emplace_back(coords);
        }
    }

    return chunks;
}

void updatePlayerChunkView(const std::shared_ptr<Player> & player, int32_t oldChunkX, int32_t oldChunkZ, int32_t newChunkX, int32_t newChunkZ) {
    // Step 1: Determine old and new viewed chunks based on view distance
    std::vector<ChunkCoordinates> oldViewedChunks{};
    if(oldChunkX != -1 || oldChunkZ != -1) {
        oldViewedChunks = getChunksInView(oldChunkX, oldChunkZ, serverConfig.viewDistance);
    }
    std::vector<ChunkCoordinates> newViewedChunks = getChunksInView(newChunkX, newChunkZ, serverConfig.viewDistance);

    // Step 2: Convert vectors to sets for easy comparison
    std::unordered_set<ChunkCoordinates> oldSet(oldViewedChunks.begin(), oldViewedChunks.end());
    std::unordered_set<ChunkCoordinates> newSet(newViewedChunks.begin(), newViewedChunks.end());

    // Step 3: Determine chunks to add and remove
    std::vector<ChunkCoordinates> chunksToAdd;
    std::vector<ChunkCoordinates> chunksToRemove;

    for (const auto & chunk : oldSet) {
        if (!newSet.contains(chunk)) {
            chunksToRemove.emplace_back(chunk);
        }
    }

    for (const auto & chunk : newSet) {
        if (!oldSet.contains(chunk)) {
            chunksToAdd.emplace_back(chunk);
        }
    }

    // Step 4: Update the chunkViewersMap
    {
        std::lock_guard lock(chunkViewersMutex);

        // Remove player from chunks no longer in view
        for (const auto & coords : chunksToRemove) {
            auto it = chunkViewersMap.find(coords);
            if (it != chunkViewersMap.end()) {
                auto & viewers = it->second;
                std::erase(viewers, player);

                // Remove the chunk from the map if no viewers remain
                if (viewers.empty()) {
                    chunkViewersMap.erase(it);
                }
            }

            // Also remove from player's current viewed chunks
            player->currentViewedChunks.erase(coords);
        }

        // Add player to new chunks
        for (const auto & coords : chunksToAdd) {
            chunkViewersMap[coords].push_back(player);

            // Add to player's current viewed chunks
            player->currentViewedChunks.emplace(coords);
        }
    }
}


// Function to determine if a block is considered for WORLD_SURFACE heightmap
bool isWorldSurface(const short& blockStateID) {
    // All blocks except air, cave air, and void air
    return  blockStateID != 0 && // air
            blockStateID != 12959 && // cave air
            blockStateID != 12958; // void air
}

std::vector<int64_t> packHeightmap(const std::vector<int64_t>& heights, int bitsPerEntry) {
    int totalBits = heights.size() * bitsPerEntry;
    int numLongs = static_cast<int>(std::ceil(static_cast<float>(totalBits) / 64.0f));

    // Ensure that numLongs is at least 37 to match client expectation
    if (numLongs < 37) {
        numLongs = 37;
    }

    std::vector<int64_t> packed(numLongs, 0);

    for (size_t i = 0; i < heights.size(); ++i) {
        int bitIndex = i * bitsPerEntry;
        int longIndex = bitIndex / 64;
        int bitOffset = bitIndex % 64;

        // Insert the height value into the packed array
        packed[longIndex] |= (heights[i] & ((1LL << bitsPerEntry) - 1)) << bitOffset;

        // Handle cases where the height value spans two longs
        if (bitOffset + bitsPerEntry > 64) {
            int bitsInNextLong = (bitOffset + bitsPerEntry) - 64;
            packed[longIndex + 1] |= (heights[i] & ((1LL << bitsInNextLong) - 1)) >> (bitsPerEntry - bitsInNextLong);
        }
    }

    return packed;
}

#ifdef _WIN32
uint64_t htobe64(uint64_t host_64bits) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return host_64bits;
#else
    return ((host_64bits & 0x00000000000000FFULL) << 56) |
           ((host_64bits & 0x000000000000FF00ULL) << 40) |
           ((host_64bits & 0x0000000000FF0000ULL) << 24) |
           ((host_64bits & 0x00000000FF000000ULL) << 8)  |
           ((host_64bits & 0x000000FF00000000ULL) >> 8)  |
           ((host_64bits & 0x0000FF0000000000ULL) >> 24) |
           ((host_64bits & 0x00FF000000000000ULL) >> 40) |
           ((host_64bits & 0xFF00000000000000ULL) >> 56);
#endif
}
#endif

// Function to unpack bitsPerEntry bits from packedData into separate indices
std::vector<uint32_t> unpackBits(const std::vector<uint8_t>& packedData, int bitsPerEntry) {
    // Validate bitsPerEntry
    if (bitsPerEntry < 1 || bitsPerEntry > 32) {
        throw std::invalid_argument("bitsPerEntry must be between 1 and 32.");
    }

    std::vector<uint32_t> unpackedData;
    size_t totalBits = packedData.size() * 8;
    size_t totalEntries = totalBits / bitsPerEntry; // Floor division
    size_t remainingBits = totalBits % bitsPerEntry;

    // Warn if there are remaining bits that don't form a complete entry
    if (remainingBits != 0) {
        logMessage("There are " + std::to_string(remainingBits) + " leftover bits that do not form a complete entry and will be ignored.", LOG_WARNING);
    }

    // Iterate over each entry
    for (size_t entry = 0; entry < totalEntries; ++entry) {
        size_t bitIndex = entry * bitsPerEntry;
        size_t byteIndex = bitIndex / 8;
        size_t bitOffset = bitIndex % 8;

        uint32_t value = 0;

        if (bitOffset + bitsPerEntry <= 8) {
            // All bits are within the current byte
            value = (packedData[byteIndex] >> bitOffset) & ((1u << bitsPerEntry) - 1);
        } else {
            // Bits are split between current byte and the next byte
            size_t bitsInFirstByte = 8 - bitOffset;
            size_t bitsInSecondByte = bitsPerEntry - bitsInFirstByte;

            // Extract bits from the first byte
            uint32_t firstPart = (packedData[byteIndex] >> bitOffset) & ((1u << bitsInFirstByte) - 1);

            // Ensure we don't go out of bounds
            if (byteIndex + 1 >= packedData.size()) {
                throw std::out_of_range("Attempting to read beyond the end of packedData.");
            }

            // Extract bits from the second byte
            uint32_t secondPart = packedData[byteIndex + 1] & ((1u << bitsInSecondByte) - 1);

            // Combine the two parts
            value = (secondPart << bitsInFirstByte) | firstPart;
        }

        unpackedData.push_back(value);
    }

    return unpackedData;
}

std::vector<uint8_t> serializeChunkSections(const std::array<std::optional<MemChunkSection>, NUM_SECTIONS>& sections) {
    std::vector<uint8_t> serializedSections;
    int count = 0;
    for (const auto& section : sections) {
        // Serialize Block Count (Short, big-endian)
        if (section.has_value()) {
            int16_t blockCountBE = htons(section.value().blockCount);
            serializedSections.push_back(reinterpret_cast<const uint8_t*>(&blockCountBE)[0]);
            serializedSections.push_back(reinterpret_cast<const uint8_t*>(&blockCountBE)[1]);
        } else {
            serializedSections.push_back(0);
            serializedSections.push_back(0);
        }

        // 1. Serialize Block States (Paletted Container)
        if (!section.has_value() || section.value().isEmpty) {
            writeByte(serializedSections, 0); // Bits Per Entry
            writeVarInt(serializedSections, blocks["air"].defaultState); // Air
            writeVarInt(serializedSections, 0); // No block states data
        } else if (section.value().palette.indexToBlockState.size() == 1) {
            // Single-valued palette
            writeByte(serializedSections, 0); // Bits Per Entry
            writeVarInt(serializedSections, section.value().palette.indexToBlockState[0]); // Single value
            writeVarInt(serializedSections, 0); // No block states data
        } else {
            // Indirect palette
            int bitsPerEntryBlock = std::max(4, calculateBitsPerEntry(section.value().palette));
            bitsPerEntryBlock = std::min(bitsPerEntryBlock, 32);
            writeByte(serializedSections, static_cast<uint8_t>(bitsPerEntryBlock)); // Bits Per Entry

            writeVarInt(serializedSections, section.value().palette.indexToBlockState.size()); // Palette Length
            for (const auto& blockID : section.value().palette.indexToBlockState) {
                writeVarInt(serializedSections, blockID); // Palette entries are blockStateIDs
            }

            // Encoded Block States using blockIndices
            size_t numberOfLongs;
            std::vector<uint32_t> unpackedBlockIndices = unpackBits(section.value().blockIndices, bitsPerEntryBlock);
            if (unpackedBlockIndices.size() != 4096) {
                logMessage("Unpacked block indices size is not 4096: " + std::to_string(unpackedBlockIndices.size()), LOG_ERROR);
            }
            std::vector<uint8_t> encodedBlockData = encodePalettedData(unpackedBlockIndices, bitsPerEntryBlock, numberOfLongs);

            // Serialize Data Array Length (VarInt)
            writeVarInt(serializedSections, static_cast<int32_t>(numberOfLongs));

            // Serialize the Data Array
            writeBytes(serializedSections, encodedBlockData);
        }

        // 2. Serialize Biomes (Paletted Container)
        if (section.value().biomePalette.indexToBlockState.size() == 1) {
            // Single-valued palette
            writeByte(serializedSections, 0); // Bits Per Entry
            writeVarInt(serializedSections, section.value().biomePalette.indexToBlockState[0]); // Single value
            writeVarInt(serializedSections, 0); // No biome data
        } else {
            // Indirect palette
            int bitsPerEntryBiome = calculateBitsPerEntry(section.value().biomePalette, 1);
            bitsPerEntryBiome = std::max(bitsPerEntryBiome, 1); // Ensure minimum 1 bit
            bitsPerEntryBiome = std::min(bitsPerEntryBiome, 32); // Cap at 32 bits

            writeByte(serializedSections, static_cast<uint8_t>(bitsPerEntryBiome)); // Bits Per Entry

            writeVarInt(serializedSections, static_cast<int32_t>(section.value().biomePalette.indexToBlockState.size())); // Palette Length
            for (const auto& biomeID : section.value().biomePalette.indexToBlockState) {
                writeVarInt(serializedSections, biomeID); // Palette entries are biomeIDs
            }

            // Encoded Biomes using biomeIndices
            size_t numberOfLongs;
            std::vector<uint32_t> unpackedBiomeIndices = unpackBits(section.value().biomeIndices, bitsPerEntryBiome);
            std::vector<uint8_t> encodedBiomes = encodePalettedData(unpackedBiomeIndices, bitsPerEntryBiome, numberOfLongs);

            // Serialize Data Array Length (VarInt)
            writeVarInt(serializedSections, static_cast<int32_t>(numberOfLongs));

            // Serialize the Data Array
            writeBytes(serializedSections, encodedBiomes);
        }

        count += 1;
    }

    return serializedSections;
}

std::vector<uint8_t> serializeBitSet(uint32_t bitMask) {
    /*
     * TODO: Test when real chunk data is available
    std::vector<uint8_t> serialized;

    // Step 1: Determine the number of longs needed
    // Since we have a 32-bit mask, we only need 1 long (64 bits)
    int32_t length = 1;
    writeVarInt(serialized, length);

    // Step 2: Serialize the bitmask as a 64-bit little-endian long
    uint64_t data = static_cast<uint64_t>(bitMask);
    for (int i = 0; i < 8; ++i) {
        serialized.push_back(static_cast<uint8_t>((data >> (i * 8)) & 0xFF));
    }

    return serialized;
    */
    std::vector<uint8_t> serializedBitSet;

    try {
        // Determine the number of longs needed (each long holds 64 bits)
        int32_t length = (bitMask != 0) ? 1 : 0; // Only add a long if any bit is set
        writeVarInt(serializedBitSet, length);

        if (length > 0) {
            // Convert the 32-bit mask to a 64-bit long
            uint64_t longData = static_cast<uint64_t>(bitMask);

            // Serialize the long in big-endian order (most significant byte first)
            for (int i = 7; i >= 0; --i) { // Big endian
                serializedBitSet.push_back(static_cast<uint8_t>((longData >> (i * 8)) & 0xFF));
            }
        }
    }
    catch (const std::exception& e) {
        logMessage("Error in serializeBitSet: " + std::string(e.what()), LOG_ERROR);
        throw; // Re-throw after logging
    }
    catch (...) {
        logMessage("Unknown error in serializeBitSet.", LOG_ERROR);
        throw; // Re-throw after logging
    }

    return serializedBitSet;
}

nbt::tag_compound serializeHeightmaps(const std::shared_ptr<Chunk> & chunk) {
    nbt::tag_compound heightmapsNBT;
    for (const auto& heightmap : chunk->heightmaps.data) {
        nbt::tag_long_array heightmapArray;
        for (auto height : heightmap.second) {
            heightmapArray.push_back(height);
        }
        heightmapsNBT[heightmap.first] = std::move(heightmapArray);
    }
    return heightmapsNBT;
}

// Function to check if a section's light data is all zero
bool isAllZero(const std::vector<uint8_t>& lightData, int sectionIndex) {
    constexpr int SECTION_SIZE = 2048; // bytes per section
    return std::all_of(lightData.begin() + sectionIndex * SECTION_SIZE,
                       lightData.begin() + (sectionIndex + 1) * SECTION_SIZE,
                       [](uint8_t byte) { return byte == 0; });
}

std::vector<uint8_t> serializeLighting(const Chunk* chunk) {
    // Initialize masks
    uint32_t skyLightMask = 0;
    uint32_t blockLightMask = 0;
    uint32_t emptySkyLightMask = 0;
    uint32_t emptyBlockLightMask = 0;

    // Iterate through each section to set masks
    for (size_t i = 0; i < chunk->sections.size(); ++i) {
        const Lighting& lighting = chunk->sections[i]->lighting;

        // Sky Light Mask
        bool hasSkyLight = std::ranges::any_of(lighting.skyLight,
                                               [](uint8_t byte) { return byte != 0; });
        if (hasSkyLight) {
            skyLightMask |= (1 << i);
            if (isAllZero(lighting.skyLight, i)) {
                emptySkyLightMask |= (1 << i);
            }
        }

        // Block Light Mask
        bool hasBlockLight = std::ranges::any_of(lighting.blockLight,
                                                 [](uint8_t byte) { return byte != 0; });
        if (hasBlockLight) {
            blockLightMask |= (1 << i);
            if (isAllZero(lighting.blockLight, i)) {
                emptyBlockLightMask |= (1 << i);
            }
        }
    }

    // Serialize Light Masks
    std::vector<uint8_t> lightMasksData;
    std::vector<uint8_t> serializedSkyLightMask = serializeBitSet(skyLightMask);
    lightMasksData.insert(lightMasksData.end(), serializedSkyLightMask.begin(), serializedSkyLightMask.end());

    std::vector<uint8_t> serializedBlockLightMask = serializeBitSet(blockLightMask);
    lightMasksData.insert(lightMasksData.end(), serializedBlockLightMask.begin(), serializedBlockLightMask.end());

    std::vector<uint8_t> serializedEmptySkyLightMask = serializeBitSet(emptySkyLightMask);
    lightMasksData.insert(lightMasksData.end(), serializedEmptySkyLightMask.begin(), serializedEmptySkyLightMask.end());

    std::vector<uint8_t> serializedEmptyBlockLightMask = serializeBitSet(emptyBlockLightMask);
    lightMasksData.insert(lightMasksData.end(), serializedEmptyBlockLightMask.begin(), serializedEmptyBlockLightMask.end());

    // Serialize Sky Light Arrays
    int32_t skyLightArrayCount = std::bitset<32>(skyLightMask).count();
    std::vector<uint8_t> skyLightArraysData;
    writeVarInt(skyLightArraysData, skyLightArrayCount);
    for (size_t i = 0; i < chunk->sections.size(); ++i) {
        if (skyLightMask & (1 << i)) {
            // Extract the sky light data for this section
            const Lighting& lighting = chunk->sections[i]->lighting;
            const std::vector<uint8_t>& skyLight = lighting.skyLight;

            // Serialize the sky light array
            writeVarInt(skyLightArraysData, 2048); // Sky Light array length

            // Append the actual sky light data
            writeBytes(skyLightArraysData, skyLight);
        }
    }

    // Serialize Block Light Arrays
    int32_t blockLightArrayCount = std::bitset<32>(blockLightMask).count();
    std::vector<uint8_t> blockLightArraysData;
    writeVarInt(blockLightArraysData, blockLightArrayCount);
    for (size_t i = 0; i < chunk->sections.size(); ++i) {
        if (blockLightMask & (1 << i)) {
            // Extract the block light data for this section
            const Lighting& lighting = chunk->sections[i]->lighting;
            const std::vector<uint8_t>& blockLight = lighting.blockLight;

            // Serialize the block light array
            writeVarInt(blockLightArraysData, 2048); // Block Light array length

            // Append the actual block light data
            writeBytes(blockLightArraysData, blockLight);
        }
    }

    // Combine all serialized data
    std::vector<uint8_t> serializedData;
    serializedData.insert(serializedData.end(), lightMasksData.begin(), lightMasksData.end());
    serializedData.insert(serializedData.end(), skyLightArraysData.begin(), skyLightArraysData.end());
    serializedData.insert(serializedData.end(), blockLightArraysData.begin(), blockLightArraysData.end());

    return serializedData;
}

// Updated serializeChunkData function including all components
std::vector<uint8_t> serializeChunkData(
    const std::shared_ptr<Chunk>& chunk
) {

     // 1. Serialize Heightmaps
    nbt::tag_compound heightmapsNBT = serializeHeightmaps(chunk);
    std::vector<uint8_t> serializedHeightmaps = serializeNBT(heightmapsNBT, true);

    // 2. Serialize Chunk Sections
    std::vector<uint8_t> serializedSections = serializeChunkSections(chunk->sections);

    // 3. Serialize Block Entities
    std::vector<uint8_t> blockEntitiesData;
    writeVarInt(blockEntitiesData, 0); // Number of block entities (0 for now)

    // 4. Serialize Light Data
    /*
     * TODO: Fix
    std::vector<uint8_t> lightData = serializeLighting(chunk.get());
    */

    uint32_t skyLightMask = 0;
    uint32_t blockLightMask = 0;
    uint32_t emptySkyLightMask = 0;
    uint32_t emptyBlockLightMask = 0;

    for (int i = 0; i < chunk->sections.size(); ++i) {
        skyLightMask |= (1 << i);
        blockLightMask |= (1 << i);
    }

    // Serialize Sky Light Mask, Block Light Mask, Empty Sky Light Mask, Empty Block Light Mask
    std::vector<uint8_t> lightMasksData;
    std::vector<uint8_t> serializedSkyLightMask = serializeBitSet(skyLightMask);
    lightMasksData.insert(lightMasksData.end(), serializedSkyLightMask.begin(), serializedSkyLightMask.end());
    std::vector<uint8_t> serializedBlockLightMask = serializeBitSet(blockLightMask);
    lightMasksData.insert(lightMasksData.end(), serializedBlockLightMask.begin(), serializedBlockLightMask.end());
    std::vector<uint8_t> serializedEmptySkyLightMask = serializeBitSet(emptySkyLightMask);
    lightMasksData.insert(lightMasksData.end(), serializedEmptySkyLightMask.begin(), serializedEmptySkyLightMask.end());
    std::vector<uint8_t> serializedEmptyBlockLightMask = serializeBitSet(emptyBlockLightMask);
    lightMasksData.insert(lightMasksData.end(), serializedEmptyBlockLightMask.begin(), serializedEmptyBlockLightMask.end());

    // Serialize Sky Light Arrays
    int32_t skyLightArrayCount = std::bitset<32>(skyLightMask).count();
    std::vector<uint8_t> skyLightArraysData;
    writeVarInt(skyLightArraysData, skyLightArrayCount);
    for (int i = 0; i < skyLightArrayCount; ++i) {
        std::vector<uint8_t> skyLightArray(2048, 0xFF); // Full light
        writeVarInt(skyLightArraysData, 2048); // Sky Light array length
        writeBytes(skyLightArraysData, skyLightArray);
    }

    // Serialize Block Light Arrays
    int32_t blockLightArrayCount = std::bitset<32>(blockLightMask).count();
    std::vector<uint8_t> blockLightArraysData;
    writeVarInt(blockLightArraysData, blockLightArrayCount);
    for (int i = 0; i < blockLightArrayCount; ++i) {
        std::vector<uint8_t> blockLightArray(2048, 0x00); // No light
        writeVarInt(blockLightArraysData, 2048); // Block Light array length
        writeBytes(blockLightArraysData, blockLightArray);
    }
    std::vector<uint8_t> lightData;
    lightData.insert(lightData.end(), lightMasksData.begin(), lightMasksData.end());
    lightData.insert(lightData.end(), skyLightArraysData.begin(), skyLightArraysData.end());
    lightData.insert(lightData.end(), blockLightArraysData.begin(), blockLightArraysData.end());

    // 5. Assemble Data Buffer
    std::vector<uint8_t> dataBuffer;
    writeBytes(dataBuffer, serializedSections);
    writeBytes(dataBuffer, blockEntitiesData);
    writeBytes(dataBuffer, lightData);

    // 6. Serialize Size VarInt
    std::vector<uint8_t> packetData;
    writeBytes(packetData, serializedHeightmaps);
    writeVarInt(packetData, static_cast<int32_t>(serializedSections.size()));
    writeBytes(packetData, dataBuffer);

    return packetData;
}


std::shared_ptr<Chunk> getChunkContainingBlock(int32_t x, int32_t y, int32_t z) {
    int32_t chunkX = getChunkCoordinate(x);
    int32_t chunkZ = getChunkCoordinate(z);

    // Access the global chunk map (thread-safe)
    std::lock_guard lock(chunkMapMutex);
    auto it = globalChunkMap.find({chunkX, chunkZ});
    if (it != globalChunkMap.end()) {
        return it->second;
    }
    return nullptr;
}

void notifyChunkUpdate(const std::shared_ptr<Chunk>& chunk, int32_t x, int32_t y, int32_t z) {
    // Construct a Block Change packet
    std::vector<uint8_t> packetData;
    packetData.push_back(0x09); // Packet ID for Block Update

    // Block Position (VarLong)
    int64_t position = encodePosition(x, y, z);
    writeLong(packetData, position);

    // Block State ID
    try {
        Block block = chunk->getBlock(getLocalCoordinate(x), y, getLocalCoordinate(z));
        writeVarInt(packetData, block.blockStateID);
    } catch (const std::out_of_range& e) {
        logMessage("notifyChunkUpdate: " + std::string(e.what()), LOG_ERROR);
        writeVarInt(packetData, 0); // Default to air if out of range
    }

    // Determine which chunks are affected (in this case, only the chunk of the block)

    // Broadcast to all players viewing this chunk
    {
        ChunkCoordinates chunkCoords{chunk->chunkX, chunk->chunkZ};
        std::lock_guard lock(chunkViewersMutex);
        auto it = chunkViewersMap.find(chunkCoords);
        if (it != chunkViewersMap.end()) {
            for (const auto& player : it->second) {
                sendPacket(*player->client, packetData);
            }
        }
    }
}

void sendChunkDataToPlayer(ClientConnection& client, const std::shared_ptr<Chunk>& chunk) {
    std::vector<uint8_t> packetData;
    packetData.push_back(0x27); // Packet ID for Chunk Data

    // Chunk X and Z
    writeInt(packetData, chunk->chunkX);
    writeInt(packetData, chunk->chunkZ);

    // Serialize the chunk data using the updated function
    std::vector<uint8_t> serializedChunkData = serializeChunkData(chunk);
    writeBytes(packetData, serializedChunkData);

    // Send the packet to the player
    sendPacket(client, packetData);
}

std::shared_ptr<Chunk> generateFlatChunk(const FlatWorldSettings& settings, int32_t chunkX, int32_t chunkZ, int& highestY) {
    // Create a new Chunk object
    std::shared_ptr<Chunk> flatChunk = std::make_shared<Chunk>(chunkX, chunkZ);

    // Initialize variables to track the current height
    int currentHeight = 0;
    highestY = currentHeight;

    // Initialize all sections as empty initially
    for (int sectionIdx = 0; sectionIdx < NUM_SECTIONS; ++sectionIdx) {
        flatChunk->sections[sectionIdx].emplace();
    }

    // Iterate through each layer in the flat world settings
    for (const auto& layer : settings.layers) {
        // Determine the start and end Y coordinates for this layer
        int layerStartY = currentHeight;
        int layerEndY = currentHeight + layer.height;

        // Clamp the layerEndY to CHUNK_HEIGHT
        if (layerEndY > CHUNK_HEIGHT) {
            layerEndY = CHUNK_HEIGHT;
        }

        // Update highestY if necessary
        if (layerEndY > highestY) {
            highestY = layerEndY;
        }

        // Iterate through each Y within the layer
        for (int y = layerStartY; y < layerEndY; ++y) {
            // Determine which section this Y belongs to
            int sectionIndex = y / SECTION_HEIGHT;

            if (sectionIndex < 0 || sectionIndex >= NUM_SECTIONS) {
                continue; // Skip invalid sections
            }

            // Get the section or create it if it doesn't exist
            auto& sectionOpt = flatChunk->sections[sectionIndex];
            MemChunkSection& section = sectionOpt.value();

            // Add the block to the palette and get its palette index
            uint8_t paletteIndex = section.palette.getIndex(blocks[stripNamespace(layer.block)].defaultState);

            // Iterate through each block in the X and Z dimensions
            for (int z = 0; z < CHUNK_LENGTH; ++z) {
                for (int x = 0; x < CHUNK_WIDTH; ++x) {
                    if (isWorldSurface(section.palette.indexToBlockState[paletteIndex])) {
                        section.blockCount++;
                        section.isEmpty = false;
                    }
                    section.tempBlockIndices.push_back(paletteIndex);
                }
            }
        }

        // Update the current height for the next layer
        currentHeight += layer.height;
        if (currentHeight >= CHUNK_HEIGHT) {
            break; // No more layers can be added beyond CHUNK_HEIGHT
        }
    }

    // Finalize each section (e.g., bit-packing, palette finalization)
    for (int sectionIdx = 0; sectionIdx < NUM_SECTIONS; ++sectionIdx) {
        auto& sectionOpt = flatChunk->sections[sectionIdx];
        if (!sectionOpt.has_value()) {
            continue; // Skip empty sections
        }

        MemChunkSection& section = sectionOpt.value();

        // Assign biome
        int defaultBiomeID = biomes[stripNamespace(settings.biome)].id;
        section.addBiome(defaultBiomeID);

        if (!section.isEmpty && section.tempBlockIndices.size() != CHUNK_WIDTH * CHUNK_LENGTH * SECTION_HEIGHT) {
            // Fill the rest of the section with air
            int airIndex = section.palette.getIndex(blocks["air"].defaultState);
            for (int i = section.tempBlockIndices.size(); i < CHUNK_WIDTH * CHUNK_LENGTH * SECTION_HEIGHT; ++i) {
                section.tempBlockIndices.push_back(airIndex);
            }
        }

        // Finalize the section
        section.finalize();

        // TODO: Calculate lighting
    }

    // TODO: Calculate heightmaps

    return flatChunk;
}

std::vector<int32_t> unpackPackedData(const std::vector<uint64_t>& packedData, int bitsPerEntry, size_t expectedCount) {
    if (bitsPerEntry > 32) throw std::runtime_error("bitsPerEntry exceeds 32");

    std::vector<int32_t> indices;
    indices.reserve(expectedCount);

    // Calculate how many indices can fit into a single 64-bit word
    int indicesPerWord = 64 / bitsPerEntry;

    for (const auto& word : packedData) {
        // Extract only the full indices that fit into the word
        for (int i = 0; i < indicesPerWord; ++i) {
            if (indices.size() >= expectedCount) break;
            int32_t index = (word >> (i * bitsPerEntry)) & ((1ULL << bitsPerEntry) - 1);
            indices.push_back(index);
        }
    }

    if (indices.size() != expectedCount) {
        throw std::runtime_error("Unpacked data size does not match expected count");
    }

    return indices;
}

std::shared_ptr<Chunk> loadChunkFromDisk(int chunkX, int chunkZ) {
    // Determine the region coordinates
    int regionX = chunkX >> 5;
    int regionZ = chunkZ >> 5;

    // Determine the local chunk coordinates within the region
    int localX = chunkX & 31;
    int localZ = chunkZ & 31;

    // Construct the region file path
    std::filesystem::path regionPath = "world/region/r." + std::to_string(regionX) + "." + std::to_string(regionZ) + ".mca"; // Replace with actual path

    // Check if the region file exists
    if (!exists(regionPath)) {
        return nullptr;
    }

    // Open the region file
    RegionFile regionFile(regionPath, true);

    // Load the chunk
    std::optional<ChunkData> chunkDataOpt = regionFile.loadChunk(localX, localZ, regionX, regionZ);
    if (!chunkDataOpt.has_value()) {
        logMessage("Chunk (" + std::to_string(chunkX) + ", " + std::to_string(chunkZ) + ") not found in region file.", LOG_WARNING);
        return nullptr;
    }

    // Extract ChunkData
    ChunkData chunkData = chunkDataOpt.value();

    // Create a new Chunk object
    std::shared_ptr<Chunk> chunk = std::make_shared<Chunk>(chunkX, chunkZ);

    // Extract Sections from NBT
    if (chunkData.nbt.has_key("sections")) {
        const nbt::tag_list& sectionsList = chunkData.nbt["sections"].as<nbt::tag_list>();
        for (const auto& sectionTag : sectionsList) {
            const auto& sectionCompound = sectionTag.as<nbt::tag_compound>();
            int8_t sectionY = sectionCompound.at("Y").as<nbt::tag_byte>().get();

            // Calculate the section index
            int sectionIndex = sectionY - MIN_Y / SECTION_HEIGHT;
            if (sectionIndex < 0 || sectionIndex >= NUM_SECTIONS) {
                logMessage("Invalid sectionY: " + std::to_string(static_cast<int>(sectionY)), LOG_ERROR);
                continue;
            }

            MemChunkSection section;
            section.isEmpty = true;

            // Extract block_states
            if (sectionCompound.has_key("block_states")) {
                const auto& blockStatesCompound = sectionCompound.at("block_states").as<nbt::tag_compound>();

                // Extract block palette
                if (blockStatesCompound.has_key("palette")) {
                    const auto& paletteList = blockStatesCompound.at("palette").as<nbt::tag_list>();
                    for (const auto& paletteEntry : paletteList) {
                        const std::string& blockName = paletteEntry.as<nbt::tag_compound>().at("Name").as<nbt::tag_string>().get();
                        int32_t blockStateID = blocks[stripNamespace(blockName)].defaultState;
                        section.palette.getIndex(blockStateID); // Populate the palette
                    }
                }

                // Extract block states data
                if (blockStatesCompound.has_key("data")) {
                    const auto& dataTag = blockStatesCompound.at("data").as<nbt::tag_long_array>();
                    const std::vector<int64_t>& packedData = dataTag.get();

                    // Convert packedData from signed chars to uint64_t
                    std::vector<uint64_t> packedData64;
                    packedData64.reserve(packedData.size());
                    for (const auto& val : packedData) {
                        packedData64.push_back(static_cast<uint64_t>(val));
                    }

                    // Determine bits per entry
                    int bitsPerEntry = calculateBitsPerEntry(section.palette);

                    // Unpack block states
                    std::vector<int32_t> blockStates = unpackPackedData(packedData64, bitsPerEntry, CHUNK_WIDTH * CHUNK_LENGTH * SECTION_HEIGHT);

                    // Assign blockStateIDs to the section
                    for (const auto& index : blockStates) {
                        if (index >= section.palette.indexToBlockState.size()) {
                            section.tempBlockIndices.push_back(0); // Air
                        } else {
                            if (isWorldSurface(section.palette.indexToBlockState[index])) {
                                section.blockCount++;
                                section.isEmpty = false;
                            }
                            section.tempBlockIndices.push_back(static_cast<uint8_t>(index));

                        }
                    }
                }
            }


            // Extract Biomes (if stored per section; otherwise, handle separately)
            if (sectionCompound.has_key("biomes")) {
                const auto& biomesCompound = sectionCompound.at("biomes").as<nbt::tag_compound>();

                // Extract biome palette
                if (biomesCompound.has_key("palette")) {
                    const auto& biomePaletteList = biomesCompound.at("palette").as<nbt::tag_list>();
                    for (const auto& biomeEntry : biomePaletteList) {
                        const std::string& biomeName = biomeEntry.as<nbt::tag_string>().get();
                        int32_t biomeID = biomes[stripNamespace(biomeName)].id;
                        section.biomePalette.getIndex(biomeID); // Populate the biome palette
                    }
                }

                // Extract biomes data
                if (biomesCompound.has_key("data")) {
                    const auto& dataTag = biomesCompound.at("data").as<nbt::tag_long_array>();
                    const std::vector<int64_t>& packedData = dataTag.get();

                    // Convert packedData from signed chars to uint64_t
                    std::vector<uint64_t> packedData64;
                    packedData64.reserve(packedData.size());
                    for (const auto& val : packedData) {
                        packedData64.push_back(static_cast<uint64_t>(val));
                    }

                    // Determine bits per entry
                    int bitsPerEntryBiome = calculateBitsPerEntry(section.biomePalette, 1);

                    // Unpack biomes
                    std::vector<int32_t> biomeIndices = unpackPackedData(packedData64, bitsPerEntryBiome, CHUNK_WIDTH / 4 * CHUNK_LENGTH / 4 * CHUNK_LENGTH / 4);

                    // Assign biome indices to the section
                    for (const auto& index : biomeIndices) {
                        if (index >= section.biomePalette.indexToBlockState.size()) {
                            section.tempBiomeIndices.push_back(0); // Default biome ID
                        } else {
                            section.tempBiomeIndices.push_back(static_cast<uint8_t>(index));
                        }
                    }
                } else {
                    // 1 biome fills the entire section
                    section.tempBiomeIndices.push_back(section.biomePalette.indexToBlockState[0]);
                }
            }

            // Extract Lighting
            Lighting lighting;
            if (sectionCompound.has_key("BlockLight")) {
                const auto& blockLightTag = sectionCompound.at("BlockLight").as<nbt::tag_byte_array>();
                lighting.blockLight = std::vector<uint8_t>(blockLightTag.get().begin(), blockLightTag.get().end());
            }
            if (sectionCompound.has_key("SkyLight")) {
                const auto& skyLightTag = sectionCompound.at("SkyLight").as<nbt::tag_byte_array>();
                lighting.skyLight = std::vector<uint8_t>(skyLightTag.get().begin(), skyLightTag.get().end());
            }
            section.lighting = lighting;

            // Finalize indices
            section.finalize();

            // Assign Blocks to Chunk
            chunk->sections[sectionIndex] = section;
        }
    }

    // Assign Heightmaps
    chunk->heightmaps = chunkData.heightmaps;

    return chunk;
}

std::shared_ptr<Chunk> getOrLoadChunk(int32_t chunkX, int32_t chunkZ) {
    ChunkCoordinates coords{chunkX, chunkZ};
    {
        std::lock_guard lock(chunkMapMutex);
        auto it = globalChunkMap.find(coords);
        if (it != globalChunkMap.end()) {
            return it->second;
        }
    }

    // Load or generate the chunk outside the lock to prevent blocking other threads
    std::shared_ptr<Chunk> chunk = loadChunkFromDisk(chunkX, chunkZ);
    if (!chunk) {
        if (serverConfig.worldType == "flat") {
            int highestY;
            FlatWorldSettings settings = flatWorldPresets[serverConfig.flatWorldPreset];
            chunk = generateFlatChunk(settings, chunkX, chunkZ, highestY);
        }
    }

    {
        std::lock_guard lock(chunkMapMutex);
        globalChunkMap[coords] = chunk;
    }

    return chunk;
}

bool sendCurrentChunkToPlayer(ClientConnection& client, int chunkX, int chunkZ) {
    auto currentChunk = getOrLoadChunk(chunkX, chunkZ);
    if (!currentChunk) {
        logMessage("Failed to load or generate current chunk for player.", LOG_ERROR);
        return false;
    }

    // Serialize and send the current chunk
    sendChunkDataToPlayer(client, currentChunk);

    globalChunkMap[ChunkCoordinates{chunkX, chunkZ}] = currentChunk;
    return true;
}

