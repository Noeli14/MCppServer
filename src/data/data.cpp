#include "data.h"

#include <fstream>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

#include "core/server.h"
#include "core/utils.h"

std::unordered_map<std::string, BiomeData> loadBiomes(const std::string& filePath) {
    std::unordered_map<std::string, BiomeData> biomeMap;
    std::ifstream file(filePath);
    if (!file.is_open()) {
        logMessage("Failed to open biomes.json file at " + filePath, LOG_ERROR);
        return biomeMap;
    }

    nlohmann::json biomesJson;
    try {
        file >> biomesJson;
    } catch (const nlohmann::json::parse_error& e) {
        logMessage("Failed to parse biomes.json: " + std::string(e.what()), LOG_ERROR);
        return biomeMap;
    }

    for (const auto& biome : biomesJson) {
        if (biome.contains("name") && biome.contains("id")) {
            BiomeData biomeData;
            std::string name = biome.value("name", "unknown");
            biomeData.id = biome.value("id", 0);
            biomeData.category = biome.value("category", "none");
            biomeData.temperature = biome.value("temperature", 0.5f);
            biomeData.hasPrecipitation = biome.value("precipitation", false);
            biomeData.dimension = biome.value("dimension", "overworld");
            biomeData.displayName = biome.value("display_name", name);
            biomeData.color = biome.value("color", 0);

            biomeMap[name] = biomeData;
        } else {
            logMessage("Biome entry missing 'name' or 'id'", LOG_ERROR);
        }
    }

    return biomeMap;
}

std::vector<uint16_t> getBlockDrops(nlohmann::basic_json<>::const_reference value) {
    std::vector<uint16_t> drops;
    if (value.is_array()) {
        for (const auto& drop : value) {
            if (drop.is_number_integer()) {
                drops.push_back(drop.get<uint16_t>());
            } else {
                logMessage("Invalid 'drops' entry in block data", LOG_ERROR);
            }
        }
    } else {
        logMessage("Invalid 'drops' entry in block data", LOG_ERROR);
    }
    return drops;
}

std::vector<uint16_t> getHarvestTools(nlohmann::basic_json<>::const_reference value) {
    std::vector<uint16_t> harvestTools;
    if (value.is_object()) {
        for (const auto& [harvestTool, mineable] : value.items()) {
            harvestTools.push_back(stoi(harvestTool));
        }
    } else {
        logMessage("Invalid 'harvestTools' entry in block data", LOG_ERROR);
    }
    return harvestTools;
}

std::unordered_map<std::string, BlockData> loadBlocks(const std::string& filePath) {
    std::unordered_map<std::string, BlockData> blockMap;
    std::ifstream file(filePath);
    if (!file.is_open()) {
        logMessage("Failed to open blocks.json file at " + filePath, LOG_ERROR);
        return blockMap;
    }

    nlohmann::json blocksJson;
    try {
        file >> blocksJson;
    } catch (const nlohmann::json::parse_error& e) {
        logMessage("Failed to parse blocks.json: " + std::string(e.what()), LOG_ERROR);
        return blockMap;
    }

    for (const auto& block : blocksJson) {
        if (block.contains("name") && block.contains("defaultState")) {
            BlockData blockData;
            std::string name = block["name"];
            blockData.id = block.value("id", 0);
            blockData.displayName = block.value("displayName", name);
            blockData.hardness = block.value("hardness", 0.0f);
            blockData.resistance = block.value("resistance", 0.0f);
            blockData.stackSize = block.value("stackSize", 64);
            blockData.diggable = block.value("diggable", true);
            blockData.material = block.value("material", "rock");
            blockData.transparent = block.value("transparent", false);
            blockData.emitLight = block.value("emitLight", 0);
            blockData.filterLight = block.value("filterLight", 0);
            blockData.defaultState = block.value("defaultState", 0);
            blockData.minStateId = block.value("minStateId", 0);
            blockData.maxStateId = block.value("maxStateId", 0);
            // Check if harvestTools exists
            if (block.contains("harvestTools")) {
                blockData.harvestTools = getHarvestTools(block["harvestTools"]);
            }
            blockData.boundingBox = block.value("boundingBox", "block");
            blockData.drops = getBlockDrops(block["drops"]);
            blockData.states = getBlockStates(block["states"]);

            blockMap[name] = blockData;
        } else {
            logMessage("Block entry missing 'name' or 'defaultState'", LOG_ERROR);
        }
    }

    return blockMap;
}

std::unordered_map<std::string, ItemData> loadItems(const std::string& filePath) {
    std::unordered_map<std::string, ItemData> itemMap;
    std::ifstream file(filePath);
    if (!file.is_open()) {
        logMessage("Failed to open items.json file at " + filePath, LOG_ERROR);
        return itemMap;
    }

    nlohmann::json itemsJson;
    try {
        file >> itemsJson;
    } catch (const nlohmann::json::parse_error& e) {
        logMessage("Failed to parse items.json: " + std::string(e.what()), LOG_ERROR);
        return itemMap;
    }

    for (const auto& item : itemsJson) {
        if (item.contains("id") && item.contains("name") && item.contains("displayName") && item.contains("stackSize")) {
            ItemData itemData;
            std::string name = item["name"];
            itemData.name = name;
            itemData.id = item.value("id", 0);
            itemData.displayName = item.value("displayName", name);
            itemData.stackSize = item.value("stackSize", 64);

            itemMap[name] = itemData;
        } else {
            logMessage("Item entry missing 'id', 'name', 'displayName' or 'stackSize'", LOG_ERROR);
        }
    }

    return itemMap;
}

std::unordered_map<int, ItemData> loadItemIDs(const std::string& filePath) {
    std::unordered_map<int, ItemData> itemMap;
    std::ifstream file(filePath);
    if (!file.is_open()) {
        logMessage("Failed to open items.json file at " + filePath, LOG_ERROR);
        return itemMap;
    }

    nlohmann::json itemsJson;
    try {
        file >> itemsJson;
    } catch (const nlohmann::json::parse_error& e) {
        logMessage("Failed to parse items.json: " + std::string(e.what()), LOG_ERROR);
        return itemMap;
    }

    for (const auto& item : itemsJson) {
        if (item.contains("id") && item.contains("name") && item.contains("displayName") && item.contains("stackSize")) {
            ItemData itemData;
            std::string name = item["name"];
            itemData.name = name;
            itemData.id = item.value("id", 0);
            itemData.displayName = item.value("displayName", name);
            itemData.stackSize = item.value("stackSize", 64);

            itemMap[itemData.id] = itemData;
        } else {
            logMessage("Item entry missing 'id', 'name', 'displayName' or 'stackSize'", LOG_ERROR);
        }
    }

    return itemMap;
}

void loadCollisions(const std::string &filePath) {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        logMessage("Failed to open collisions JSON file: " + filePath, LOG_ERROR);
        return;
    }

    nlohmann::json j;
    try {
        file >> j;
    } catch (const nlohmann::json::parse_error& e) {
        logMessage("JSON parse error in " + filePath + ": " + e.what(), LOG_ERROR);
        return;
    }

    // Parse blocks
    if (j.contains("blocks") && j["blocks"].is_object()) {
        for (auto& [key, value] : j["blocks"].items()) {
            if (value.is_number_integer()) {
                blockNameToShapeIDs[key] = {value.get<int>()};
            }
            // Handle blocks with arrays of shape IDs
            else if (value.is_array()) {
                for (const auto& var : value) {
                    if (var.is_number_integer()) {
                        blockNameToShapeIDs[key].emplace_back(var);
                    }
                }
            }
        }
    } else {
        logMessage("No 'blocks' section found in " + filePath, LOG_ERROR);
    }

    // Parse shapes
    if (j.contains("shapes") && j["shapes"].is_object()) {
        for (auto& [key, value] : j["shapes"].items()) {
            int blockID = std::stoi(key);
            if (value.is_array()) {
                for (const auto& shape : value) {
                    if (shape.is_array() && shape.size() == 6) {
                        BoundingBox bbox{};
                        bbox.minX = shape[0].get<double>();
                        bbox.minY = shape[1].get<double>();
                        bbox.minZ = shape[2].get<double>();
                        bbox.maxX = shape[3].get<double>();
                        bbox.maxY = shape[4].get<double>();
                        bbox.maxZ = shape[5].get<double>();
                        shapeIDToShapes[blockID].emplace_back(bbox);
                    }
                }
            }
        }
    } else {
        logMessage("No 'shapes' section found in " + filePath, LOG_ERROR);
    }
}

std::unordered_map<std::string, std::vector<int>> loadBlockTags(std::unordered_map<std::string, BlockData>& blocks, const std::string& filePath) {
    std::unordered_map<std::string, std::vector<int>> tagMap;

    std::ifstream file(filePath);
    if (!file.is_open()) {
        logMessage("Failed to open block_tags JSON file: " + filePath, LOG_ERROR);
        return {};
    }

    nlohmann::json j;
    try {
        file >> j;
    } catch (const nlohmann::json::parse_error& e) {
        logMessage("JSON parse error in " + filePath + ": " + e.what(), LOG_ERROR);
        return {};
    }

    if (j.is_object()) {
        for (auto& [tag, value] : j.items()) {
            if (value.is_array()) {
                for (const auto& block : value) {
                    if (block.is_string()) {
                        std::string blockName = block.get<std::string>();
                        if (blocks.contains(blockName)) {
                            tagMap[tag].push_back(blocks[blockName].id);
                        } else {
                            logMessage("Block not found for tag: " + blockName, LOG_ERROR);
                        }
                    }
                }
            }
        }
    }

    return tagMap;
}

std::unordered_map<std::string, std::vector<int>> loadItemTags(std::unordered_map<std::string, ItemData>& items, const std::string& filePath) {
    std::unordered_map<std::string, std::vector<int>> tagMap;

    std::ifstream file(filePath);
    if (!file.is_open()) {
        logMessage("Failed to open item_tags JSON file: " + filePath, LOG_ERROR);
        return {};
    }

    nlohmann::json j;
    try {
        file >> j;
    } catch (const nlohmann::json::parse_error& e) {
        logMessage("JSON parse error in " + filePath + ": " + e.what(), LOG_ERROR);
        return {};
    }

    if (j.is_object()) {
        for (auto& [tag, value] : j.items()) {
            if (value.is_array()) {
                for (const auto& item : value) {
                    if (item.is_string()) {
                        auto itemName = item.get<std::string>();
                        if (items.contains(itemName)) {
                            tagMap[tag].push_back(items[itemName].id);
                        } else {
                            logMessage("Item not found for tag: " + itemName, LOG_ERROR);
                        }
                    }
                }
            }
        }
    }

    return tagMap;
}