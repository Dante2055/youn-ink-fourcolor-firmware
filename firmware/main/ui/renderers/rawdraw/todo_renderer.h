/**
 * @file todo_renderer.h
 * @brief Todo list page renderer for rawdraw mode
 *
 * Renders interactive todo list with checkboxes, item selection,
 * and completion toggle.
 */

#ifndef RAWDRAW_TODO_RENDERER_H
#define RAWDRAW_TODO_RENDERER_H

#include "ui/renderers/rawdraw/page_renderer.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/style.h"
#include "rawdraw/theme.h"

#include <string>
#include <vector>

namespace rawdraw {

struct TodoItem {
    std::string text;
    bool completed = false;
};

class TodoRenderer : public PageRenderer {
public:
    TodoRenderer();
    ~TodoRenderer() override;

    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;

    void SetItems(const std::vector<TodoItem>& items);
    void AddItem(const std::string& text, bool completed = false);
    void ToggleItem(size_t index);
    void RemoveItem(size_t index);
    void Clear();

    size_t GetSelectedIndex() const { return selected_index_; }
    size_t GetItemCount() const { return items_.size(); }
    const std::vector<TodoItem>& GetItems() const { return items_; }

private:
    std::vector<TodoItem> items_;
    size_t selected_index_ = 0;
    size_t top_visible_index_ = 0;
    int width_ = Style::kScreenWidth;
    int height_ = Style::kScreenHeight;
    const lv_font_t* font_ = &SourceHanSansSC_Regular_slim;
};

}  // namespace rawdraw

#endif  // RAWDRAW_TODO_RENDERER_H
