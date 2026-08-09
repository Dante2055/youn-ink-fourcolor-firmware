/**
 * @file photo_gallery.cc
 * @brief Memory-style gallery renderer for rawdraw mode
 */

#include "photo_gallery.h"
#include "common/photo_storage.h"
#include "rawdraw/components/footer_bar.h"
#include "rawdraw/layout_utils.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/style.h"
#include "rawdraw/theme.h"

#include <esp_log.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

extern const lv_font_t SourceHanSansSC_Regular_slim;
extern const lv_font_t SourceHanSansSC_Medium_slim;
extern const lv_font_t font_zectrix_16_1;

namespace rawdraw {

namespace {

static const char* kTag = "PhotoGallery";

std::string FitTextToWidth(const std::string& text, const lv_font_t* font, int max_width) {
    if (!font || max_width <= 0 || text.empty()) return "";
    if (MeasureTextWidth(text.c_str(), font) <= max_width) return text;

    static const std::string kEllipsis = "...";
    const int ellipsis_w = MeasureTextWidth(kEllipsis.c_str(), font);
    if (ellipsis_w >= max_width) return "";

    std::string fitted;
    const char* p = text.c_str();
    while (*p) {
        const char* start = p;
        uint32_t ch = utf8_next(&p);
        if (ch == 0) break;

        std::string next = fitted;
        next.append(start, p - start);
        next += kEllipsis;
        if (MeasureTextWidth(next.c_str(), font) > max_width) break;
        fitted.append(start, p - start);
    }
    fitted += kEllipsis;
    return fitted;
}

std::vector<std::string> WrapText(const std::string& text, const lv_font_t* font,
                                  int max_width, size_t max_lines) {
    std::vector<std::string> lines;
    if (!font || max_width <= 0 || text.empty()) return lines;

    std::string current;
    const char* p = text.c_str();
    while (*p) {
        const char* start = p;
        uint32_t ch = utf8_next(&p);
        if (ch == 0) break;

        if (ch == '\n') {
            if (!current.empty()) lines.push_back(current);
            current.clear();
            if (lines.size() >= max_lines) break;
            continue;
        }

        std::string next = current;
        next.append(start, p - start);
        if (!current.empty() && MeasureTextWidth(next.c_str(), font) > max_width) {
            lines.push_back(current);
            current.assign(start, p - start);
            if (lines.size() >= max_lines) break;
        } else {
            current = std::move(next);
        }
    }

    if (lines.size() < max_lines && !current.empty()) {
        lines.push_back(current);
    }
    if (lines.size() > max_lines) {
        lines.resize(max_lines);
    }
    if (lines.size() == max_lines && p && *p) {
        lines.back() = FitTextToWidth(lines.back() + "...", font, max_width);
    }
    return lines;
}

int BytesPerRow1bpp(int width) {
    return std::max(1, (width + 7) / 8);
}

int BytesPerRow2bpp(int width) {
    return std::max(1, (width + 3) / 4);
}

bool IsBwry2bppImage(int width, int height, uint32_t size) {
    return width > 0 && height > 0 && size >= static_cast<uint32_t>(BytesPerRow2bpp(width) * height);
}

bool IsMono1bppImage(int width, int height, uint32_t size) {
    return width > 0 && height > 0 && size >= static_cast<uint32_t>(BytesPerRow1bpp(width) * height);
}

Color ReadPhotoPixelColor(const uint8_t* data, uint32_t size, int photo_width, bool bwry2bpp,
                          int src_x, int src_y) {
    if (!data || photo_width <= 0 || src_x < 0 || src_y < 0) return BLACK;
    if (bwry2bpp) {
        const int bpr = BytesPerRow2bpp(photo_width);
        const int offset = src_y * bpr + (src_x >> 2);
        if (offset < 0 || offset >= static_cast<int>(size)) return BLACK;
        const int shift = 6 - ((src_x & 0x03) * 2);
        const uint8_t color = (data[offset] >> shift) & 0x03;
        return static_cast<Color>(color);
    }

    const int bpr = BytesPerRow1bpp(photo_width);
    const int offset = src_y * bpr + (src_x >> 3);
    if (offset < 0 || offset >= static_cast<int>(size)) return BLACK;
    const int bit = 7 - (src_x & 0x07);
    return ((data[offset] >> bit) & 0x01) != 0 ? WHITE : BLACK;
}

}  // namespace

PhotoGalleryRenderer::PhotoGalleryRenderer()
    : mode_(kMemoryCardMode)
    , selected_index_(0)
    , showing_delete_dialog_(false)
    , delete_dialog_selected_(1)
    , current_photo_data_(nullptr)
    , current_photo_size_(0)
    , current_photo_width_(400)
    , current_photo_height_(300)
    , font_(&SourceHanSansSC_Regular_slim)
    , title_font_(&SourceHanSansSC_Medium_slim)
    , icon_font_(&font_zectrix_16_1) {
}

PhotoGalleryRenderer::~PhotoGalleryRenderer() {
    if (current_photo_data_) {
        free(current_photo_data_);
        current_photo_data_ = nullptr;
    }
}

void PhotoGalleryRenderer::Init(int width, int height) {
    width_ = width;
    height_ = height;
    needs_full_refresh_ = true;
    mode_ = kMemoryCardMode;
    selected_index_ = 0;
    showing_delete_dialog_ = false;
    delete_dialog_selected_ = 1;
    RefreshPhotoList();
}

void PhotoGalleryRenderer::Render(uint8_t* fb, int width, int height) {
    if (!fb) return;
    switch (mode_) {
        case kMemoryCardMode:
            RenderMemoryCardMode(fb, width, height);
            break;
        case kFullscreenMode:
            RenderFullscreenMode(fb, width, height);
            break;
    }
    if (showing_delete_dialog_) {
        RenderDeleteDialog(fb, width, height);
    }
    needs_full_refresh_ = false;
}

bool PhotoGalleryRenderer::HandleInput(const ButtonEvent& event) {
    if (showing_delete_dialog_) {
        switch (event.type) {
            case ButtonEvent::kUpClick:
            case ButtonEvent::kDownClick:
                delete_dialog_selected_ = delete_dialog_selected_ == 0 ? 1 : 0;
                needs_full_refresh_ = true;
                return true;
            case ButtonEvent::kBootClick:
                if (delete_dialog_selected_ == 0) {
                    DeleteSelectedPhoto();
                }
                showing_delete_dialog_ = false;
                delete_dialog_selected_ = 1;
                needs_full_refresh_ = true;
                return true;
            case ButtonEvent::kBootLongPress:
            case ButtonEvent::kBootDoubleClick:
                showing_delete_dialog_ = false;
                delete_dialog_selected_ = 1;
                needs_full_refresh_ = true;
                return true;
            default:
                return true;
        }
    }

    switch (event.type) {
        case ButtonEvent::kUpClick:
            if (selected_index_ > 0) {
                selected_index_--;
                ClampSelection();
                if (mode_ == kFullscreenMode) LoadPhotoData(selected_index_);
                needs_full_refresh_ = true;
                return true;
            }
            break;
        case ButtonEvent::kDownClick:
            if (selected_index_ < GetPhotoCount() - 1) {
                selected_index_++;
                ClampSelection();
                if (mode_ == kFullscreenMode) LoadPhotoData(selected_index_);
                needs_full_refresh_ = true;
                return true;
            }
            break;
        case ButtonEvent::kBootClick:
            if (mode_ == kMemoryCardMode) {
                ClampSelection();
                LoadPhotoData(selected_index_);
                mode_ = kFullscreenMode;
            } else {
                mode_ = kMemoryCardMode;
            }
            needs_full_refresh_ = true;
            return true;
        case ButtonEvent::kBootDoubleClick:
            if (!photo_ids_.empty()) {
                showing_delete_dialog_ = true;
                delete_dialog_selected_ = 1;
                needs_full_refresh_ = true;
                return true;
            }
            break;
        default:
            break;
    }
    return false;
}

void PhotoGalleryRenderer::RefreshPhotoList() {
    photo_ids_.clear();

    PhotoInfo info;
    int count = photo_get_count();
    for (int i = 0; i < count && i < PHOTO_MAX_PHOTOS; i++) {
        if (photo_get_by_index(i, &info) == 0) {
            PhotoEntry entry = {};
            memcpy(entry.id, info.id, sizeof(entry.id));
            memcpy(entry.title, info.title, sizeof(entry.title));
            memcpy(entry.date, info.date, sizeof(entry.date));
            memcpy(entry.location, info.location, sizeof(entry.location));
            memcpy(entry.body, info.body, sizeof(entry.body));
            entry.width = info.width;
            entry.height = info.height;
            entry.file_size = info.file_size;
            photo_ids_.push_back(entry);
        }
    }
    ClampSelection();
    ESP_LOGI(kTag, "RefreshPhotoList: storage_count=%d visible_count=%d selected=%d",
             count, static_cast<int>(photo_ids_.size()), selected_index_);
}

void PhotoGalleryRenderer::SetSelectedIndex(int index) {
    if (photo_ids_.empty()) {
        selected_index_ = 0;
        return;
    }
    selected_index_ = std::max(0, std::min(index, static_cast<int>(photo_ids_.size()) - 1));
    if (mode_ == kFullscreenMode) {
        LoadPhotoData(selected_index_);
    }
}

bool PhotoGalleryRenderer::SetSelectedById(const char* id) {
    if (!id || id[0] == '\0') return false;
    for (int i = 0; i < static_cast<int>(photo_ids_.size()); ++i) {
        if (strcmp(photo_ids_[i].id, id) == 0) {
            SetSelectedIndex(i);
            return true;
        }
    }
    return false;
}

void PhotoGalleryRenderer::EnterFullscreenMode() {
    if (photo_ids_.empty()) return;
    showing_delete_dialog_ = false;
    mode_ = kFullscreenMode;
    LoadPhotoData(selected_index_);
}

bool PhotoGalleryRenderer::SelectNext(bool wrap) {
    const int count = GetPhotoCount();
    if (count <= 1) return false;

    int next = selected_index_ + 1;
    if (next >= count) {
        if (!wrap) return false;
        next = 0;
    }

    SetSelectedIndex(next);
    needs_full_refresh_ = true;
    ESP_LOGI(kTag, "Slideshow next photo: %d/%d", selected_index_ + 1, count);
    return true;
}

bool PhotoGalleryRenderer::IsCurrentPhotoBwry2bpp() const {
    return IsBwry2bppImage(current_photo_width_, current_photo_height_, current_photo_size_);
}

void PhotoGalleryRenderer::RenderMemoryCardMode(uint8_t* fb, int width, int height) {
    const int content_top = Style::kStatusBarHeight + Style::kSpacingSM;
    const int footer_h = Style::kFooterBarHeight;
    const int body_h = height - content_top - footer_h;
    const int gap = 8;
    const int info_x = 12;

    // Compute adaptive left panel width based on content (chat bubble style)
    // Min width for empty state, max width to leave enough space for photo
    const int left_w_min = 120;
    const int left_w_max = 200;
    int left_w = left_w_min;

    if (!photo_ids_.empty()) {
        const PhotoEntry& entry = photo_ids_[selected_index_];
        // Estimate required width from title + body + meta block
        const std::string title_text = entry.title[0] ? entry.title : "那年今日";
        const std::string body_text = entry.body[0] ? entry.body : "暂无文案";
        const std::string date_text = entry.date[0] ? entry.date : "日期未知";
        const std::string location_text = entry.location[0] ? entry.location : "地点未知";

        int max_text_w = 0;
        // Title lines (2 max)
        auto title_lines = WrapText(title_text, title_font_, left_w_max - 24, 2);
        for (const auto& line : title_lines) {
            max_text_w = std::max(max_text_w, MeasureTextWidth(line.c_str(), title_font_));
        }
        // Body lines (5 max)
        auto body_lines = WrapText(body_text, font_, left_w_max - 24, 5);
        for (const auto& line : body_lines) {
            max_text_w = std::max(max_text_w, MeasureTextWidth(line.c_str(), font_));
        }
        // Meta block (date + location)
        max_text_w = std::max(max_text_w, MeasureTextWidth(date_text.c_str(), font_));
        max_text_w = std::max(max_text_w, MeasureTextWidth(location_text.c_str(), font_));

        // Add padding: chip (64), padding (24 each side)
        left_w = std::min(left_w_max, std::max(left_w_min, max_text_w + 24));
    }

    const int photo_x = info_x + left_w + gap;
    const int photo_w = width - photo_x - 12;
    const int card_y = content_top + 2;
    const int card_h = body_h - 4;

    const auto& theme = ThemeManager::Get();
    const PaintStyle bg_style = theme.Style(ThemeToken::BackgroundPrimary);
    PaintStyle white_card_style;
    white_card_style.fg = BLACK;
    white_card_style.bg = WHITE;
    white_card_style.border = BLACK;
    white_card_style.border_width = 1;
    const PaintStyle badge_style = theme.Style(ThemeToken::Badge);
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);

    DrawStyledRect(fb, width, {0, content_top, width, body_h}, bg_style);
    DrawStyledRoundRect(fb, width, height, {info_x, card_y, left_w, card_h},
                        Style::kBorderRadiusMD, white_card_style);

    if (photo_ids_.empty()) {
        const char* title = "暂无回忆";
        const char* body = "通过 /api/push_memory 推送图片和文案";
        const int safe_x = info_x + 12;
        const int safe_w = left_w - 24;
        const int title_box_y = card_y + 34;
        const int body_box_y = title_box_y + 34;
        DrawText(fb, width, safe_x,
                 InkCenteredTextTopYInBox(title_font_, title, title_box_y, 24, 0),
                 title, title_font_, text);
        auto empty_lines = WrapText(body, font_, safe_w, 4);
        int line_box_y = body_box_y;
        for (const auto& line : empty_lines) {
            DrawText(fb, width, safe_x,
                     InkCenteredTextTopYInBox(font_, line.c_str(), line_box_y, 22, 0),
                     line.c_str(), font_, secondary);
            line_box_y += 24;
        }
        RenderPhotoInRect(fb, width, PhotoEntry{}, photo_x, card_y, photo_w, card_h, false);
        FooterBar footer;
        footer.SetBounds(width, height);
        footer.SetText("UP上一张", nullptr, "BOOT看详情");
        footer.Draw(fb, width, height);
        return;
    }

    const PhotoEntry& entry = photo_ids_[selected_index_];

    // -----------------------------------------------------------
    // User Design Layout: Calendar card style with date & week
    // -----------------------------------------------------------
    time_t now_time = time(nullptr);
    struct tm tm_info;
    localtime_r(&now_time, &tm_info);

    char mon_buf[16];
    snprintf(mon_buf, sizeof(mon_buf), "%d月", tm_info.tm_mon + 1);

    char day_buf[16];
    snprintf(day_buf, sizeof(day_buf), "%d", tm_info.tm_mday);

    static const char* kWeekDays[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
    const char* week_str = kWeekDays[tm_info.tm_wday % 7];

    const int center_x = info_x + left_w / 2;

    // 1. Top Section: "8月" flanked by yellow dots & lines on the exact same center axis
    const int mon_line_y = card_y + 28;
    int mon_first_ofs = 0;
    const int mon_ink_w = MeasureTextScaledInkWidth(mon_buf, title_font_, 1, &mon_first_ofs);
    const int mon_x = center_x - mon_ink_w / 2 - mon_first_ofs;
    const int mon_top_y = InkCenteredTextTopY(title_font_, mon_buf, mon_line_y, 0);
    DrawText(fb, width, mon_x, mon_top_y, mon_buf, title_font_, BLACK);

    const int mon_dot_left_x = (center_x - mon_ink_w / 2) - 12;
    const int mon_dot_right_x = (center_x + mon_ink_w / 2) + 12;
    DrawCircle(fb, width, {mon_dot_left_x, mon_line_y}, 2, YELLOW);
    DrawCircle(fb, width, {mon_dot_right_x, mon_line_y}, 2, YELLOW);

    const int line_left_start = info_x + 10;
    const int line_right_end = info_x + left_w - 10;
    if (mon_dot_left_x - 5 > line_left_start) {
        DrawRect(fb, width, {line_left_start, mon_line_y, mon_dot_left_x - 5 - line_left_start, 1}, YELLOW);
    }
    if (line_right_end > mon_dot_right_x + 5) {
        DrawRect(fb, width, {mon_dot_right_x + 5, mon_line_y, line_right_end - (mon_dot_right_x + 5), 1}, YELLOW);
    }

    // 2. Middle Section: Giant RED Day Number "9" (120号字 = 24px × 5倍矢量放缩)
    constexpr int kDayScale = 5;
    int day_first_ofs = 0;
    const int day_ink_w = MeasureTextScaledInkWidth(day_buf, title_font_, kDayScale, &day_first_ofs);
    const int day_x = center_x - day_ink_w / 2 - day_first_ofs;
    const int day_y = 105;
    DrawTextScaled(fb, width, day_x, day_y, day_buf, title_font_, RED, kDayScale, height);
    DrawTextScaled(fb, width, day_x + 1, day_y, day_buf, title_font_, RED, kDayScale, height);

    // 3. Bottom Section: Yellow Divider with Center Dot & "星期日"
    const int btm_line_y = card_y + card_h - 48;
    DrawCircle(fb, width, {center_x, btm_line_y}, 2, YELLOW);
    if (center_x - 6 > line_left_start) {
        DrawRect(fb, width, {line_left_start, btm_line_y, center_x - 6 - line_left_start, 1}, YELLOW);
    }
    if (line_right_end > center_x + 6) {
        DrawRect(fb, width, {center_x + 6, btm_line_y, line_right_end - (center_x + 6), 1}, YELLOW);
    }

    const int week_y = btm_line_y + 10;
    int week_first_ofs = 0;
    const int week_ink_w = MeasureTextScaledInkWidth(week_str, font_, 1, &week_first_ofs);
    const int week_x = center_x - week_ink_w / 2 - week_first_ofs;
    DrawText(fb, width, week_x, InkCenteredTextTopYInBox(font_, week_str, week_y, 22, 0),
             week_str, font_, BLACK);

    const int frame_x = photo_x;
    const int frame_y = card_y;
    const int frame_w = photo_w;
    const int frame_h = card_h;
    RenderPhotoInRect(fb, width, entry, frame_x, frame_y, frame_w, frame_h, false);

    FooterBar footer;
    footer.SetBounds(width, height);
    char counter[40];
    snprintf(counter, sizeof(counter), "%d/%d", selected_index_ + 1, GetPhotoCount());
    footer.SetText("UP/DN翻页", counter, "BOOT看详情");
    footer.Draw(fb, width, height);
}

void PhotoGalleryRenderer::RenderPhotoInRect(uint8_t* fb, int fb_width, const PhotoEntry& entry,
                                             int x, int y, int w, int h, bool invert) {
    const auto& theme = ThemeManager::Get();
    PaintStyle frame_style = invert ? theme.Style(ThemeToken::Selected)
                                    : theme.Component(ComponentRole::CardDefault);
    DrawStyledRoundRect(fb, fb_width, 300, {x, y, w, h}, Style::kBorderRadiusSM, frame_style);

    if (entry.file_size == 0) {
        const char* label = "无图片";
        int tw = MeasureTextWidth(label, font_);
        // FIX: 改用 InkCenteredTextTopYInBox，避免 line_height 居中导致中文偏上
        // 参见 wiki/projects/notellm-baseline-alignment.md
        DrawText(fb, fb_width, x + (w - tw) / 2, InkCenteredTextTopYInBox(font_, label, y, h, 0),
                 label, font_, frame_style.fg);
        return;
    }

    uint8_t* photo_buf = static_cast<uint8_t*>(malloc(entry.file_size));
    if (!photo_buf) {
        return;
    }
    int bytes_read = photo_load(entry.id, photo_buf, entry.file_size);
    if (bytes_read <= 0 || entry.width <= 0 || entry.height <= 0) {
        free(photo_buf);
        return;
    }

    const bool bwry2bpp = IsBwry2bppImage(entry.width, entry.height, bytes_read);
    const bool mono1bpp = IsMono1bppImage(entry.width, entry.height, bytes_read);
    if (!bwry2bpp && !mono1bpp) {
        free(photo_buf);
        return;
    }

    const int inner_x = x + 4;
    const int inner_y = y + 4;
    const int inner_w = w - 8;
    const int inner_h = h - 8;

    // Cover mode: photo fills the entire area (width or height touches edges)
    // Choose scale that makes photo cover the whole rect, may crop overflow
    const int draw_w_by_h = entry.height > 0 ? (inner_h * entry.width) / entry.height : inner_w;
    const int draw_h_by_w = entry.width > 0 ? (inner_w * entry.height) / entry.width : inner_h;

    // Use max to fill at least one dimension completely (cover mode)
    const int draw_w = std::max(inner_w, draw_w_by_h);
    const int draw_h = std::max(inner_h, draw_h_by_w);

    // Center the photo, overflow will be clipped by the frame border
    const int draw_x = inner_x + (inner_w - draw_w) / 2;
    const int draw_y = inner_y + (inner_h - draw_h) / 2;

    // Render with nearest-neighbor scaling
    for (int ty = 0; ty < draw_h; ++ty) {
        int src_y = (ty * entry.height) / draw_h;
        if (src_y >= entry.height) break;
        for (int tx = 0; tx < draw_w; ++tx) {
            int src_x = (tx * entry.width) / draw_w;
            if (src_x >= entry.width) break;
            Color src_color = ReadPhotoPixelColor(photo_buf, bytes_read, entry.width, bwry2bpp, src_x, src_y);
            // Only draw if inside the visible frame (clip overflow)
            if (draw_x + tx >= inner_x && draw_x + tx < inner_x + inner_w &&
                draw_y + ty >= inner_y && draw_y + ty < inner_y + inner_h) {
                set_pixel(fb, fb_width, draw_x + tx, draw_y + ty, src_color);
            }
        }
    }
    free(photo_buf);
}

void PhotoGalleryRenderer::RenderFullscreenMode(uint8_t* fb, int width, int height) {
    const auto& theme = ThemeManager::Get();
    DrawStyledRect(fb, width, {0, 0, width, height}, theme.Style(ThemeToken::BackgroundPrimary));

    if (photo_ids_.empty() || !current_photo_data_ || current_photo_size_ == 0) {
        const char* label = "无法加载照片";
        int tw = MeasureTextWidth(label, font_);
        DrawText(fb, width, (width - tw) / 2, height / 2, label, font_,
                 theme.ColorFor(ThemeToken::TextPrimary));
        return;
    }

    const bool bwry2bpp = IsBwry2bppImage(current_photo_width_, current_photo_height_, current_photo_size_);
    const int photo_byte_width = bwry2bpp ? BytesPerRow2bpp(current_photo_width_)
                                          : BytesPerRow1bpp(current_photo_width_);
    const int expected_rows = (bwry2bpp || IsMono1bppImage(current_photo_width_, current_photo_height_, current_photo_size_))
                                  ? std::min<int>(current_photo_height_, current_photo_size_ / photo_byte_width)
                                  : 0;
    int start_y = (height - expected_rows) / 2;
    if (start_y < 0) start_y = 0;

    const int draw_w = std::min(width, current_photo_width_);
    const int start_x = std::max(0, (width - draw_w) / 2);
    for (int row = 0; row < expected_rows && (start_y + row) < height; row++) {
        for (int tx = 0; tx < draw_w; ++tx) {
            const Color src_color = ReadPhotoPixelColor(current_photo_data_, current_photo_size_,
                                                        current_photo_width_, bwry2bpp, tx, row);
            set_pixel(fb, width, start_x + tx, start_y + row, src_color);
        }
    }

}

void PhotoGalleryRenderer::RenderDeleteDialog(uint8_t* fb, int width, int height) {
    if (!fb) return;
    const auto& theme = ThemeManager::Get();
    const PaintStyle bg_style = theme.Style(ThemeToken::BackgroundPrimary);
    const PaintStyle modal_style = theme.Component(ComponentRole::Modal);
    const PaintStyle shadow_style = theme.Style(ThemeToken::Shadow);
    const PaintStyle selected_style = theme.Component(ComponentRole::ButtonSelected);
    const PaintStyle danger_style = theme.Component(ComponentRole::ButtonDanger);
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);
    const Color border = theme.ColorFor(ThemeToken::Border);
    const Color danger = theme.ColorFor(ThemeToken::Danger);

    const int dialog_w = 292;
    const int dialog_h = 156;
    const int dialog_x = (width - dialog_w) / 2;
    const int dialog_y = (height - dialog_h) / 2;
    const int titlebar_h = 28;
    const int shadow_offset = 2;

    DrawStyledRoundRect(fb, width, height, {dialog_x - 4, dialog_y - 4, dialog_w + 10, dialog_h + 10},
                        Style::kBorderRadiusMD, bg_style);
    DrawStyledRoundRect(fb, width, height, {dialog_x + shadow_offset, dialog_y + shadow_offset, dialog_w, dialog_h},
                        Style::kBorderRadiusMD, shadow_style);
    DrawStyledRoundRect(fb, width, height, {dialog_x, dialog_y, dialog_w, dialog_h},
                        Style::kBorderRadiusMD, modal_style);
    DrawHLine(fb, width, dialog_y + titlebar_h, dialog_x + 1, dialog_x + dialog_w - 2, border);
    DrawRectBorder(fb, width, {dialog_x + 8, dialog_y + 8, 12, 12}, 1, danger);
    DrawLine(fb, width, {dialog_x + 10, dialog_y + 10}, {dialog_x + 18, dialog_y + 18}, danger);
    DrawLine(fb, width, {dialog_x + 18, dialog_y + 10}, {dialog_x + 10, dialog_y + 18}, danger);

    const char* title = "删除照片";
    const int title_w = MeasureTextWidth(title, font_);
    DrawText(fb, width, dialog_x + (dialog_w - title_w) / 2,
             InkCenteredTextTopYInBox(font_, title, dialog_y, titlebar_h, 0),
             title, font_, danger, height);
    for (int yy = dialog_y + 6; yy < dialog_y + titlebar_h - 5; yy += 4) {
        DrawHLine(fb, width, yy, dialog_x + 28, dialog_x + (dialog_w - title_w) / 2 - 8, border);
        DrawHLine(fb, width, yy, dialog_x + (dialog_w + title_w) / 2 + 8, dialog_x + dialog_w - 12, border);
    }

    const char* body = "确认删除当前照片？";
    const int body_w = MeasureTextWidth(body, title_font_);
    DrawText(fb, width, dialog_x + (dialog_w - body_w) / 2,
             InkCenteredTextTopYInBox(title_font_, body, dialog_y + titlebar_h + 18, 28, 0),
             body, title_font_, text, height);

    const char* labels[] = {"删除", "取消"};
    const int button_y = dialog_y + 94;
    const int button_w = 92;
    const int button_h = 30;
    const int gap = 18;
    const int start_x = dialog_x + (dialog_w - button_w * 2 - gap) / 2;
    for (int i = 0; i < 2; ++i) {
        const int x = start_x + i * (button_w + gap);
        const bool selected = delete_dialog_selected_ == i;
        const PaintStyle style = selected ? (i == 0 ? danger_style : selected_style)
                                         : modal_style;
        DrawStyledRoundRect(fb, width, height, {x, button_y, button_w, button_h},
                            Style::kBorderRadiusSM, style);
        const int label_w = MeasureTextWidth(labels[i], font_);
        DrawText(fb, width, x + (button_w - label_w) / 2,
                 InkCenteredTextTopYInBox(font_, labels[i], button_y, button_h, 0),
                 labels[i], font_, style.fg, height);
    }

    const char* hint = "UP/DN 切换  BOOT 确认";
    const int hint_w = MeasureTextWidth(hint, font_);
    DrawText(fb, width, dialog_x + (dialog_w - hint_w) / 2,
             InkCenteredTextTopYInBox(font_, hint, dialog_y + dialog_h - 24, 20, 0),
             hint, font_, secondary, height);
}

void PhotoGalleryRenderer::LoadPhotoData(int index) {
    if (current_photo_data_) {
        free(current_photo_data_);
        current_photo_data_ = nullptr;
    }
    current_photo_size_ = 0;

    if (index < 0 || index >= static_cast<int>(photo_ids_.size())) return;

    PhotoInfo info;
    if (photo_get_by_index(index, &info) != 0) return;

    current_photo_data_ = static_cast<uint8_t*>(malloc(info.file_size));
    if (!current_photo_data_) return;

    int bytes_read = photo_load(info.id, current_photo_data_, info.file_size);
    if (bytes_read > 0) {
        current_photo_size_ = bytes_read;
        current_photo_width_ = info.width;
        current_photo_height_ = info.height;
    } else {
        free(current_photo_data_);
        current_photo_data_ = nullptr;
    }
}

void PhotoGalleryRenderer::DeleteSelectedPhoto() {
    if (selected_index_ < 0 || selected_index_ >= static_cast<int>(photo_ids_.size())) return;
    char id[16] = {};
    strlcpy(id, photo_ids_[selected_index_].id, sizeof(id));
    if (photo_delete(id) == 0) {
        ESP_LOGI(kTag, "Deleted photo id=%s", id);
        RefreshPhotoList();
        ClampSelection();
        if (mode_ == kFullscreenMode) {
            if (!photo_ids_.empty()) {
                LoadPhotoData(selected_index_);
            } else {
                mode_ = kMemoryCardMode;
            }
        }
    } else {
        ESP_LOGW(kTag, "Failed to delete photo id=%s", id);
    }
}

void PhotoGalleryRenderer::ClampSelection() {
    int count = GetPhotoCount();
    if (count == 0) {
        selected_index_ = 0;
    } else if (selected_index_ >= count) {
        selected_index_ = count - 1;
    } else if (selected_index_ < 0) {
        selected_index_ = 0;
    }
}

}  // namespace rawdraw
