#ifndef CRAFTING_INVENTORY_H
#define CRAFTING_INVENTORY_H
#include <cstdint>

#include "inventory.h"


struct SlotData;

class CraftingInventory : public Inventory {
public:
    int32_t lastCraftedRecipeId = -1;
    // Dimensions of the crafting grid
    int craftWidth;
    int craftHeight;

    CraftingInventory(int16_t size, int cw, int ch)
        : Inventory(size), craftWidth(cw), craftHeight(ch) {}

    void UpdateCraftingResult();
    void ConsumeCraftingIngredients();
    bool FindShapedRecipePlacement(const CraftingRecipe &recipe, const std::vector<uint16_t> &inputItems, int craftWidth, int craftHeight, int &outXOff, int &outYOff) const;
    void OnCraftingSlotChanged();
    int32_t CraftableRecipe();
    bool FindMatchingRecipe(const std::vector<uint16_t> &inputItems, SlotData &resultSlotData) const;
    bool MatchesRecipe(const CraftingRecipe &recipe, const std::vector<uint16_t> &inputItems, int craftWidth, int craftHeight) const;
    bool MatchesShapelessRecipe(const CraftingRecipe &recipe, const std::vector<uint16_t> &inputItems) const;
    bool MatchesShapedRecipe(const CraftingRecipe &recipe, const std::vector<uint16_t> &inputItems, int craftWidth, int craftHeight) const;
    bool CheckShapedRecipePlacement(const CraftingRecipe &recipe, const std::vector<uint16_t> &inputItems, int craftWidth, int craftHeight, int xOff, int yOff) const;

};



#endif //CRAFTING_INVENTORY_H
