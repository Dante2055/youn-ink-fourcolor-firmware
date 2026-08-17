/**
 * @file todo_renderer.cc
 * @brief Todo list page renderer for rawdraw mode
 */

#include "todo_renderer.h"
#include "rawdraw/layout_utils.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/style.h"
#include "rawdraw/theme.h"

#include <algorithm>
#include <cstdio>

namespace rawdraw {

TodoRenderer::TodoRenderer()
    : font_(&SourceHanSansSC_Medium_slim) {
    // Default initial items if none provided
    items_ = {
        {"选豆泡豆", true},
        {"蒸煮熟化", false},
        {"第一次翻晒", true},
        {"第二次翻晒", false},
        {"第三次翻晒", false},
    };
}

TodoRenderer::~TodoRenderer() = default;

void TodoRenderer::Init(int width, int height) {
    width_ = width;
    height_ = height;
    needs_full_refresh_ = true;
    selected_index_ = 0;
    top_visible_index_ = 0;
}

void TodoRenderer::SetItems(const std::vector<TodoItem>& items) {
    items_ = items;
    selected_index_ = 0;
    top_visible_index_ = 0;
    needs_full_refresh_ = true;
}

void TodoRenderer::AddItem(const std::string& text, bool completed) {
    items_.push_back({text, completed});
    needs_full_refresh_ = true;
}

void TodoRenderer::ToggleItem(size_t index) {
    if (index < items_.size()) {
        items_[index].completed = !items_[index].completed;
        needs_full_refresh_ = true;
    }
}

void TodoRenderer::RemoveItem(size_t index) {
    if (index < items_.size()) {
        items_.erase(items_.begin() + index);
        if (selected_index_ >= items_.size() && !items_.empty()) {
            selected_index_ = items_.size() - 1;
        }
        needs_full_refresh_ = true;
    }
}

void TodoRenderer::Clear() {
    items_.clear();
    selected_index_ = 0;
    top_visible_index_ = 0;
    needs_full_refresh_ = true;
}

void TodoRenderer::Render(uint8_t* fb, int width, int height) {
    if (!fb) return;

    const int content_top = Style::kStatusBarHeight + 6;
    const int item_height = 38;
    const int max_visible = 6;

    if (items_.empty()) {
        const char* empty_text = "暂无待办事项";
        int tw = MeasureTextWidth(empty_text, font_);
        int th = MeasureTextHeight(font_);
        int tx = (width - tw) / 2;
        int ty = content_top + (height - content_top - th) / 2;
        DrawText(fb, width, tx, ty, empty_text, font_, ThemeManager::Get().ColorFor(ThemeToken::TextSecondary));
        needs_full_refresh_ = false;
        return;
    }

    // Clamp selected index
    if (selected_index_ >= items_.size()) {
        selected_index_ = items_.empty() ? 0 : items_.size() - 1;
    }

    // Scroll viewport adjust
    if (selected_index_ < top_visible_index_) {
        top_visible_index_ = selected_index_;
    } else if (selected_index_ >= top_visible_index_ + max_visible) {
        top_visible_index_ = selected_index_ - max_visible + 1;
    }

    int y = content_top;
    size_t end_idx = std::min(items_.size(), top_visible_index_ + static_cast<size_t>(max_visible));

    for (size_t i = top_visible_index_; i < end_idx; ++i) {
        const bool is_selected = (i == selected_index_);
        const bool is_completed = items_[i].completed;

        Rect row_r = {
            Style::kTodoItemPadding,
            y,
            width - 2 * Style::kTodoItemPadding,
            item_height - 4
        };

        // Background / Focus highlight (Pure white background with border highlight & left indicator)
        if (is_selected) {
            DrawRoundRect(fb, width, row_r, 4, WHITE, BLACK, 2);
            FillRect(fb, width, {row_r.x, row_r.y + 4, 4, row_r.h - 8}, BLACK);
        }

        // Checkbox (Uncompleted = Hollow box ☐, Completed = Solid fill ■)
        const int cb_size = 20;
        const int cb_x = row_r.x + 12;
        const int cb_y = row_r.y + (row_r.h - cb_size) / 2;

        if (is_completed) {
            FillRect(fb, width, {cb_x, cb_y, cb_size, cb_size}, BLACK);
        } else {
            DrawRect(fb, width, {cb_x, cb_y, cb_size, cb_size}, BLACK);
        }

        // Text (24pt)
        const int text_x = cb_x + 30;
        const int text_y = InkCenteredTextTopYInBox(font_, items_[i].text.c_str(), row_r.y, row_r.h, 0);

        Color text_color = is_completed ? ThemeManager::Get().ColorFor(ThemeToken::TextSecondary) : BLACK;

        DrawText(fb, width, text_x, text_y, items_[i].text.c_str(), font_, text_color);

        // Strikethrough line for completed items
        if (is_completed) {
            int text_w = MeasureTextWidth(items_[i].text.c_str(), font_);
            const int strike_y = row_r.y + row_r.h / 2;
            DrawLine(fb, width, Point{text_x, strike_y}, Point{text_x + text_w, strike_y}, text_color);
        }

        y += item_height;
    }

    needs_full_refresh_ = false;
}

bool TodoRenderer::HandleInput(const ButtonEvent& event) {
    if (items_.empty()) return false;

    switch (event.type) {
        case ButtonEvent::kUpClick:
        case ButtonEvent::kUpLongPress:
            if (selected_index_ > 0) {
                selected_index_--;
            } else {
                selected_index_ = items_.size() - 1;
            }
            needs_full_refresh_ = true;
            return true;

        case ButtonEvent::kDownClick:
        case ButtonEvent::kDownLongPress:
            if (selected_index_ + 1 < items_.size()) {
                selected_index_++;
            } else {
                selected_index_ = 0;
            }
            needs_full_refresh_ = true;
            return true;

        case ButtonEvent::kBootClick:
            if (selected_index_ < items_.size()) {
                items_[selected_index_].completed = !items_[selected_index_].completed;
                needs_full_refresh_ = true;
            }
            return true;

        default:
            break;
    }
    return false;
}

}  // namespace rawdraw
