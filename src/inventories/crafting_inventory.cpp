#include "crafting_inventory.h"

#include <array>

#include "core/server.h"
#include "data/crafting_recipes.h"
#include "entities/slot_data.h"

// Updates the crafting result slot (0) based on what's in the crafting grid.
void CraftingInventory::UpdateCraftingResult() {
    int totalSlots = craftWidth * craftHeight;
    std::vector<uint16_t> inputItems(totalSlots);
    for (int i = 0; i < totalSlots; i++) {
        SlotData &slot = getSlotData(i+1);
        inputItems[i] = slot.itemId.value();
    }

    SlotData resultSlotData;
    resultSlotData.itemId = 0;
    resultSlotData.itemCount = 0;

    if (FindMatchingRecipe(inputItems, resultSlotData)) {
        SlotData &resultSlot = getSlotData(0);
        resultSlot = resultSlotData;
    } else {
        clearSlot(0);
    }
}

bool CraftingInventory::FindMatchingRecipe(const std::vector<uint16_t> &inputItems, SlotData &resultSlotData) const {
    for (auto &pair : craftingRecipes) {
        const CraftingRecipe &recipe = pair.second;
        if (MatchesRecipe(recipe, inputItems, craftWidth, craftHeight)) {
            resultSlotData.itemId = recipe.result;
            resultSlotData.itemCount = recipe.resultCount;
            return true;
        }
    }
    return false;
}


bool CraftingInventory::MatchesRecipe(const CraftingRecipe &recipe, const std::vector<uint16_t> &inputItems, int craftWidth, int craftHeight) const {
    if (!recipe.shapeless) {
        // Shaped recipe
        return MatchesShapedRecipe(recipe, inputItems, craftWidth, craftHeight);
    } else {
        // Shapeless recipe
        return MatchesShapelessRecipe(recipe, inputItems);
    }
}

bool CraftingInventory::MatchesShapelessRecipe(const CraftingRecipe &recipe, const std::vector<uint16_t> &inputItems) const {
    // Create a copy of input items to track used items
    std::vector<uint16_t> remainingItems = inputItems;

    // Iterate over each ingredient in the recipe
    for (const auto &ingredientOptions : recipe.ingredients) {
        bool matched = false;

        for (const auto& ingredient : ingredientOptions) {
            if (std::holds_alternative<uint16_t>(ingredient)) {
                // Ingredient is an item ID
                uint16_t requiredItem = std::get<uint16_t>(ingredient);
                auto it = std::find(remainingItems.begin(), remainingItems.end(), requiredItem);
                if (it != remainingItems.end()) {
                    matched = true;
                    // Remove the matched item
                    *it = 0;
                    break;
                }
            } else if (std::holds_alternative<std::string>(ingredient)) {
                // Ingredient is a tag
                std::string tagName = std::get<std::string>(ingredient);
                if (itemTags.find(tagName) != itemTags.end()) {
                    const std::vector<int> &tagBlocks = itemTags.at(tagName);
                    auto it = std::find_if(remainingItems.begin(), remainingItems.end(),
                                           [&](uint16_t itemId) {
                                               return itemId != 0 && std::find(tagBlocks.begin(), tagBlocks.end(), itemId) != tagBlocks.end();
                                           });
                    if (it != remainingItems.end()) {
                        matched = true;
                        *it = 0; // matched, clear item
                        break;
                    }
                } else {
                    logMessage("Unknown tag: " + tagName, LOG_WARNING);
                }
            }
        }

        if (!matched) {
            // Required ingredient not found
            return false;
        }
    }

    // Ensure no extra items remain for shapeless recipes
    for (auto item : remainingItems) {
        if (item != 0) {
            // Extra items present, fail
            return false;
        }
    }

    // All ingredients matched and no extras
    return true;
}

bool CraftingInventory::MatchesShapedRecipe(const CraftingRecipe &recipe, const std::vector<uint16_t> &inputItems, int craftWidth, int craftHeight) const {
    if (recipe.width > craftWidth || recipe.height > craftHeight) return false;

    // Possible offsets
    for (int yOff = 0; yOff <= craftHeight - recipe.height; yOff++) {
        for (int xOff = 0; xOff <= craftWidth - recipe.width; xOff++) {
            if (CheckShapedRecipePlacement(recipe, inputItems, craftWidth, craftHeight, xOff, yOff)) {
                return true;
            }
        }
    }

    return false;
}

bool CraftingInventory::CheckShapedRecipePlacement(const CraftingRecipe &recipe, const std::vector<uint16_t> &inputItems, int craftWidth, int craftHeight, int xOff, int yOff) const {
    auto getInput = [&](int x, int y) -> uint16_t {
        int index = x + y * craftWidth;
        return inputItems[index];
    };

    // Check every cell of the recipe
    for (int ry = 0; ry < recipe.height; ry++) {
        for (int rx = 0; rx < recipe.width; rx++) {
            const auto &ingredientOptions = recipe.ingredients[rx + ry * recipe.width];
            uint16_t actual = getInput(rx + xOff, ry + yOff);

            bool matched = false;
            if (ingredientOptions.empty() && actual == 0) {
                continue; // Empty cell
            }
            for (const auto& ingredient : ingredientOptions) {
                if (std::holds_alternative<uint16_t>(ingredient)) {
                    uint16_t requiredItem = std::get<uint16_t>(ingredient);
                    if (actual == requiredItem) {
                        matched = true;
                        break;
                    }
                }
                else if (std::holds_alternative<std::string>(ingredient)) {
                    std::string tagName = std::get<std::string>(ingredient);
                    if (itemTags.find(tagName) != itemTags.end()) {
                        const std::vector<int> &tagBlocks = itemTags.at(tagName);
                        if (std::find(tagBlocks.begin(), tagBlocks.end(), actual) != tagBlocks.end()) {
                            matched = true;
                            break;
                        }
                    }
                    else {
                        logMessage("Unknown tag: " + tagName, LOG_WARNING);
                    }
                }
            }

            if (!matched) {
                return false;
            }
        }
    }

    // Check other slots not in recipe are empty
    for (int y = 0; y < craftHeight; y++) {
        for (int x = 0; x < craftWidth; x++) {
            bool inRecipeArea = (x >= xOff && x < xOff + recipe.width &&
                                 y >= yOff && y < yOff + recipe.height);
            if (!inRecipeArea) {
                if (getInput(x, y) != 0) {
                    return false;
                }
            }
        }
    }

    return true;
}

void CraftingInventory::ConsumeCraftingIngredients() {
    int totalSlots = craftWidth * craftHeight;

    std::vector<std::pair<uint16_t,int>> indexedItems;
    indexedItems.reserve(totalSlots);
    for (int i = 0; i < totalSlots; i++) {
        SlotData &slot = getSlotData(i + 1);
        indexedItems.push_back({slot.itemId.value(), i+1});
    }

    std::vector<uint16_t> inputItems(totalSlots);
    for (int i = 0; i < totalSlots; i++) {
        SlotData &slot = getSlotData(i + 1);
        inputItems[i] = slot.itemId.value();
    }

    CraftingRecipe matchedRecipe;
    bool found = false;
    int xOff = 0, yOff = 0;

    for (auto &pair : craftingRecipes) {
        const CraftingRecipe &recipe = pair.second;
        if (MatchesRecipe(recipe, inputItems, craftWidth, craftHeight)) {
            lastCraftedRecipeId = pair.first;
            matchedRecipe = recipe;
            found = true;
            break;
        }
    }

    if (!found) return;

    if (!matchedRecipe.shapeless) {
        // Shaped recipe
        if (!FindShapedRecipePlacement(matchedRecipe, inputItems, craftWidth, craftHeight, xOff, yOff)) {
            return; // Should not happen if MatchesRecipe was true
        }

        // Consume ingredients
        for (int ry = 0; ry < matchedRecipe.height; ry++) {
            for (int rx = 0; rx < matchedRecipe.width; rx++) {
                const auto &ingredientOptions = matchedRecipe.ingredients[rx + ry * matchedRecipe.width];
                uint16_t actual = getSlotData((xOff + rx) + (yOff + ry) * craftWidth + 1).itemId.value();

                bool consumed = false;
                for (const auto& ingredient : ingredientOptions) {
                    if (std::holds_alternative<uint16_t>(ingredient)) {
                        uint16_t requiredItem = std::get<uint16_t>(ingredient);
                        if (actual == requiredItem) {
                            SlotData &sd = getSlotData((xOff + rx) + (yOff + ry) * craftWidth + 1);
                            if (sd.itemCount > 0) {
                                sd.itemCount--;
                                if (sd.itemCount == 0) {
                                    sd.itemId = 0;
                                }
                            }
                            consumed = true;
                            break;
                        }
                    }
                    else if (std::holds_alternative<std::string>(ingredient)) {
                        std::string tagName = std::get<std::string>(ingredient);
                        if (itemTags.find(tagName) != itemTags.end()) {
                            const std::vector<int> &tagBlocks = itemTags.at(tagName);
                            if (std::find(tagBlocks.begin(), tagBlocks.end(), actual) != tagBlocks.end()) {
                                SlotData &sd = getSlotData((xOff + rx) + (yOff + ry) * craftWidth + 1);
                                if (sd.itemCount > 0) {
                                    sd.itemCount--;
                                    if (sd.itemCount == 0) {
                                        sd.itemId = 0;
                                    }
                                }
                                consumed = true;
                                break;
                            }
                        }
                        else {
                            logMessage("Unknown tag: " + tagName, LOG_WARNING);
                        }
                    }
                }

                if (!consumed && actual != 0) {
                    logMessage("Failed to consume ingredient at slot: " + std::to_string((xOff + rx) + (yOff + ry) * craftWidth + 1), LOG_WARNING);
                }
            }
        }
    } else {
        // Shapeless recipe
       std::vector<std::pair<uint16_t,int>> remainingItems = indexedItems;

        for (const auto &ingredientOptions : matchedRecipe.ingredients) {
            bool matched = false;

            for (const auto& ingredient : ingredientOptions) {
                if (std::holds_alternative<uint16_t>(ingredient)) {
                    uint16_t requiredItem = std::get<uint16_t>(ingredient);
                    auto it = std::find_if(remainingItems.begin(), remainingItems.end(),
                                           [&](const std::pair<uint16_t,int>& p) {
                                               return p.first == requiredItem;
                                           });
                    if (it != remainingItems.end()) {
                        // Consume from the corresponding slot
                        SlotData &sd = getSlotData(it->second);
                        if (sd.itemCount > 0) {
                            sd.itemCount--;
                            if (sd.itemCount == 0) {
                                sd.itemId = 0;
                            }
                            remainingItems.erase(it);
                            matched = true;
                            break;
                        }
                    }
                } else if (std::holds_alternative<std::string>(ingredient)) {
                    std::string tagName = std::get<std::string>(ingredient);
                    if (itemTags.find(tagName) == itemTags.end()) {
                        logMessage("Unknown tag: " + tagName, LOG_WARNING);
                        continue;
                    }
                    const std::vector<int> &tagBlocks = itemTags.at(tagName);

                    auto it = std::find_if(remainingItems.begin(), remainingItems.end(),
                                           [&](const std::pair<uint16_t,int>& p) {
                                               return std::find(tagBlocks.begin(), tagBlocks.end(), p.first) != tagBlocks.end();
                                           });
                    if (it != remainingItems.end()) {
                        // Consume from the corresponding slot
                        SlotData &sd = getSlotData(it->second);
                        if (sd.itemCount > 0) {
                            sd.itemCount--;
                            if (sd.itemCount == 0) {
                                sd.itemId = 0;
                            }
                            remainingItems.erase(it);
                            matched = true;
                            break;
                        }
                    }
                }
            }

            if (!matched) {
                logMessage("Failed to consume a required ingredient for shapeless recipe.", LOG_WARNING);
                return;
            }
        }
    }
}


bool CraftingInventory::FindShapedRecipePlacement(const CraftingRecipe &recipe, const std::vector<uint16_t> &inputItems, int craftWidth, int craftHeight, int &outXOff, int &outYOff) const {
    if (recipe.width > craftWidth || recipe.height > craftHeight) return false;

    for (int yOff = 0; yOff <= craftHeight - recipe.height; yOff++) {
        for (int xOff = 0; xOff <= craftWidth - recipe.width; xOff++) {
            if (CheckShapedRecipePlacement(recipe, inputItems, craftWidth, craftHeight, xOff, yOff)) {
                outXOff = xOff;
                outYOff = yOff;
                return true;
            }
        }
    }
    return false;
}


void CraftingInventory::OnCraftingSlotChanged() {
    UpdateCraftingResult();
}

int32_t CraftingInventory::CraftableRecipe() {
    int totalSlots = craftWidth * craftHeight;
    std::vector<uint16_t> inputItems(totalSlots);
    for (int i = 0; i < totalSlots; i++) {
        SlotData &slot = getSlotData(i+1);
        inputItems[i] = slot.itemId.value();
    }

    for (auto &pair : craftingRecipes) {
        const CraftingRecipe &recipe = pair.second;
        if (MatchesRecipe(recipe, inputItems, craftWidth, craftHeight)) {
            return pair.first;
        }
    }
    return -1;
}
