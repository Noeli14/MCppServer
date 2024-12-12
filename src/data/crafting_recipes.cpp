#include "crafting_recipes.h"

#include <fstream>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

#include "core/server.h"
#include "core/utils.h"
#include "entities/slot_data.h"
#include "enums/enums.h"
#include "networking/network.h"

std::vector<uint8_t> CraftingRecipe::serialize(uint16_t id) const {
    std::vector<uint8_t> data;
    writeString(data, std::to_string(id));
    writeVarInt(data, shapeless ? 1 : 0);
    writeString(data, "Craft"); // TODO: Write group, need to write own extractor for this
    writeVarInt(data, 0); // Building = 0, Redstone = 1, Equipment = 2, Misc = 3 TODO: Write type, also extractor needed

    if (!shapeless) {
        // Shaped
        writeVarInt(data, width);
        writeVarInt(data, height);

        // Ingredients: width*height
        if (ingredients.size() != width * height) {
            logMessage("Invalid ingredient count for crafting recipe", LOG_WARNING);
            return {};
        }
        for (auto ing : ingredients) {
            writeVarInt(data, 1); // Count
            SlotData slotData;
            //slotData.itemId = ing; TODO: Implement this
            slotData.itemCount = 1;
            writeSlotSimple(data, slotData);
        }

        // Result (Slot)
        SlotData slotData;
        slotData.itemId = result;
        slotData.itemCount = resultCount;
        writeSlotSimple(data, slotData);

        // Show notification
        writeByte(data, false);// TODO: Write showNotification, need to write own extractor for this
    } else {
        // Shapeless
        writeVarInt(data, static_cast<int>(ingredients.size()));
        for (auto ing : ingredients) {
            writeVarInt(data, 1); // Count
            SlotData slotData;
            //slotData.itemId = ing; TODO: Implement this
            slotData.itemCount = 1;
            writeSlotSimple(data, slotData);
        }

        // Result (Slot)
        SlotData slotData;
        slotData.itemId = result;
        slotData.itemCount = resultCount;
        writeSlotSimple(data, slotData);
    }

    return data;
}

std::unordered_multimap<uint16_t, CraftingRecipe> loadCraftingRecipes(const std::string &filename) {
    std::unordered_multimap<uint16_t, CraftingRecipe> craftingRecipes;
    std::ifstream file(filename);
    if (!file) {
        logMessage("Failed to open crafting recipe file: " + filename, LOG_ERROR);
        return craftingRecipes;
    }
    try {
        // Parse the JSON file
        nlohmann::json j;
        file >> j;

        if (!j.is_array()) {
            logMessage("Crafting recipe JSON is not an array.", LOG_ERROR);
            return craftingRecipes;
        }

        uint16_t recipeId = 1; // Starting ID for recipes

        for (const auto& recipeVariant : j) {
            CraftingRecipe craftingRecipe;

            // Extract result
            if (!recipeVariant.contains("result") || !recipeVariant["result"].is_object()) {
                logMessage("Recipe missing 'result' field or 'result' is not an object.", LOG_WARNING);
                continue;
            }

            if (!recipeVariant["result"].contains("id") || !recipeVariant["result"]["id"].is_string()) {
                logMessage("Recipe 'result' missing 'id' field or 'id' is not a string.", LOG_WARNING);
                continue;
            }

            if (!recipeVariant["result"].contains("count") || !recipeVariant["result"]["count"].is_number_unsigned()) {
                logMessage("Recipe 'result' missing 'count' field or 'count' is not unsigned number.", LOG_WARNING);
                continue;
            }

            std::string resultIdStr = recipeVariant["result"]["id"].get<std::string>();
            craftingRecipe.result = items[stripNamespace(resultIdStr)].id;
            craftingRecipe.resultCount = recipeVariant["result"]["count"].get<uint8_t>();

            // Determine if recipe is shaped or shapeless
            if (!recipeVariant.contains("shaped") || !recipeVariant["shaped"].is_boolean()) {
                logMessage("Recipe missing 'shaped' field or 'shaped' is not a boolean.", LOG_WARNING);
                continue;
            }

            craftingRecipe.shapeless = !recipeVariant["shaped"].get<bool>();

            if (!craftingRecipe.shapeless) {
                // Shaped recipe
                // Extract width and height
                if (!recipeVariant.contains("pattern") || !recipeVariant["pattern"].is_array()) {
                    logMessage("Shaped recipe missing 'pattern' field or 'pattern' is not an array.", LOG_WARNING);
                    continue;
                }

                const auto& pattern = recipeVariant["pattern"];
                craftingRecipe.height = static_cast<uint8_t>(pattern.size());
                if (craftingRecipe.height > 0) {
                    craftingRecipe.width = static_cast<uint8_t>(pattern[0].get<std::string>().size());
                }
                else {
                    craftingRecipe.width = 0;
                }

                // Extract the key mapping
                if (!recipeVariant.contains("key") || !recipeVariant["key"].is_object()) {
                    logMessage("Shaped recipe missing 'key' field or 'key' is not an object.", LOG_WARNING);
                    continue;
                }

                const auto& key = recipeVariant["key"];
                std::unordered_map<char, std::vector<std::variant<uint16_t, std::string>>> keyMapping; // Map character to list of item IDs or tag names

                for (const auto& [k, v] : key.items()) {
                    if (k.size() != 1) {
                        logMessage("Invalid key symbol: " + k, LOG_WARNING);
                        continue;
                    }
                    char symbol = k[0];
                    std::vector<std::variant<uint16_t, std::string>> options;

                    if (v.is_object()) {
                        // Single item or tag
                        if (v.contains("item") && v["item"].is_string()) {
                            std::string itemIdStr = v["item"].get<std::string>();
                            uint16_t itemId = items[stripNamespace(itemIdStr)].id;
                            options.emplace_back(itemId);
                        }
                        else if (v.contains("tag") && v["tag"].is_string()) {
                            std::string tagName = v["tag"].get<std::string>();
                            options.emplace_back(tagName);
                        }
                        else {
                            logMessage("Key '" + k + "' missing 'item' or 'tag' field or they are not strings.", LOG_WARNING);
                            continue;
                        }
                    }
                    else if (v.is_array()) {
                        // Multiple items or tags
                        for (const auto& option : v) {
                            if (option.is_object()) {
                                if (option.contains("item") && option["item"].is_string()) {
                                    std::string itemIdStr = option["item"].get<std::string>();
                                    uint16_t itemId = items[stripNamespace(itemIdStr)].id;
                                    options.emplace_back(itemId);
                                }
                                else if (option.contains("tag") && option["tag"].is_string()) {
                                    std::string tagName = option["tag"].get<std::string>();
                                    options.emplace_back(tagName);
                                }
                                else {
                                    logMessage("Key '" + k + "' has an option missing 'item' or 'tag' field or they are not strings.", LOG_WARNING);
                                    continue;
                                }
                            }
                            else {
                                logMessage("Key '" + k + "' has a non-object option.", LOG_WARNING);
                                continue;
                            }
                        }
                    }
                    else {
                        logMessage("Key '" + k + "' has an invalid format.", LOG_WARNING);
                        continue;
                    }

                    keyMapping[symbol] = options;
                }

                // Iterate over the pattern and map to ingredients
                craftingRecipe.ingredients.clear();
                for (const auto& row : pattern) {
                    std::string patternRow = row.get<std::string>();
                    for (char symbol : patternRow) {
                        if (symbol == ' ') {
                            // Assuming ' ' represents an empty slot
                            craftingRecipe.ingredients.emplace_back(std::vector<std::variant<uint16_t, std::string>>{0}); // ID 0 (empty)
                        }
                        else {
                            if (keyMapping.find(symbol) != keyMapping.end()) {
                                craftingRecipe.ingredients.emplace_back(keyMapping[symbol]);
                            }
                            else {
                                logMessage("Symbol '" + std::string(1, symbol) + "' not found in key mapping.", LOG_WARNING);
                                craftingRecipe.ingredients.emplace_back(std::vector<std::variant<uint16_t, std::string>>{0}); // Assign empty or handle as needed
                            }
                        }
                    }
                }

            } else {
                // Shapeless recipe
                if (!recipeVariant.contains("ingredients") || !recipeVariant["ingredients"].is_array()) {
                    logMessage("Item: " + recipeVariant["result"]["id"].get<std::string>() + ". Shapeless recipe missing 'ingredients' field or 'ingredients' is not an array.", LOG_WARNING);
                    continue;
                }

                craftingRecipe.ingredients.clear();
                for (const auto& ingredient : recipeVariant["ingredients"]) {
                    std::vector<std::variant<uint16_t, std::string>> options;

                    if (ingredient.is_object()) {
                        if (ingredient.contains("item") && ingredient["item"].is_string()) {
                            std::string itemIdStr = ingredient["item"].get<std::string>();
                            uint16_t itemId = items[stripNamespace(itemIdStr)].id;
                            options.emplace_back(itemId);
                        }
                        else if (ingredient.contains("tag") && ingredient["tag"].is_string()) {
                            std::string tagName = ingredient["tag"].get<std::string>();
                            options.emplace_back(tagName);
                        }
                        else {
                            logMessage("Item: " + recipeVariant["result"]["id"].get<std::string>() + ". Ingredient missing 'item' or 'tag' field or they are not strings.", LOG_WARNING);
                            options.emplace_back(static_cast<uint16_t>(0));
                        }
                    }
                    else if (ingredient.is_array()) {
                        // Multiple options for this ingredient
                        for (const auto& option : ingredient) {
                            if (option.is_object()) {
                                if (option.contains("item") && option["item"].is_string()) {
                                    std::string itemIdStr = option["item"].get<std::string>();
                                    uint16_t itemId = items[stripNamespace(itemIdStr)].id;
                                    options.emplace_back(itemId);
                                }
                                else if (option.contains("tag") && option["tag"].is_string()) {
                                    std::string tagName = option["tag"].get<std::string>();
                                    options.emplace_back(tagName);
                                }
                                else {
                                    logMessage("Item: " + recipeVariant["result"]["id"].get<std::string>() + ". Ingredient option missing 'item' or 'tag' field or they are not strings.", LOG_WARNING);
                                    continue;
                                }
                            }
                            else {
                                logMessage("Item: " + recipeVariant["result"]["id"].get<std::string>() + ". Ingredient option is not an object.", LOG_WARNING);
                                continue;
                            }
                        }
                    }
                    else {
                        logMessage("Item: " + recipeVariant["result"]["id"].get<std::string>() + ". Ingredient has an invalid format.", LOG_WARNING);
                        options.emplace_back(static_cast<uint16_t>(0));
                    }

                    craftingRecipe.ingredients.emplace_back(options);
                }
            }

            // TODO: Handle other fields like "category", "group" if necessary

            // Assign a unique ID and insert into the map
            craftingRecipes.emplace(recipeId, craftingRecipe);
            recipeId++;

            // Optional: Check for ID overflow
            if (recipeId == 0) { // Wrapped around
                logMessage("Recipe ID overflow occurred.", LOG_ERROR);
                break;
            }
        }

    }
    catch (const nlohmann::json::parse_error &e) {
        logMessage("Failed to parse crafting recipe file: " + std::string(e.what()), LOG_ERROR);
    }

    return craftingRecipes;
}
