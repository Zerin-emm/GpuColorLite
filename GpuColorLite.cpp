// ============================================================================
//  GpuColorLite.cpp —— NVIDIA 显卡 RGB 灯效控制工具
//
//  单文件实现：
//    · Direct2D / DirectWrite 全自绘 UI（无系统控件、无 XAML、无外部运行时）
//    · NVAPI I2C 直写 GPU 灯效寄存器
//    · 注册表持久化（RGB + HSV，灰色/黑色也能精确恢复）
//
//  窗口特性：
//    · 无系统标题栏与边框：WS_POPUP | WS_THICKFRAME + WM_NCCALCSIZE
//    · 保留 DWM 阴影与圆角：DWMWA_WINDOW_CORNER_PREFERENCE
//    · 启动/关闭动画：完全交给系统 DWM，不自行绘制
//    · 跟随系统深浅色（AppsUseLightTheme + WM_SETTINGCHANGE）
//
//  代码分区：
//    1. 常量与通用工具
//    2. Theme       —— 主题色板
//    3. ColorUtil   —— HSV/RGB 转换
//    4. Draw        —— D2D 绘制辅助
//    5. UrlLauncher —— 系统 Launcher 打开链接
//    6. GpuColor    —— NVAPI 后端 + 注册表
//    7. AboutWindow —— 关于窗口
//    8. MainWindow  —— 主取色窗口
//    9. wWinMain    —— 入口与命令行
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "windowsapp.lib")

// ============================================================================
//  1. 常量与通用工具
// ============================================================================

namespace Constant
{
    // 主窗口逻辑画布（16:9）
    constexpr float CanvasWidth = 640.0f;
    constexpr float CanvasHeight = 360.0f;

    // 色相环
    constexpr float RingCenterX = 168.0f;
    constexpr float RingCenterY = 196.0f;
    constexpr float RingInner = 94.0f;
    constexpr float RingOuter = 120.0f;
    constexpr float SquareSize = 124.0f;

    // 关于窗口
    constexpr float AboutWidth = 420.0f;
    constexpr float AboutHeight = 300.0f;

    // 动画时长（毫秒）
    constexpr UINT OpenAnimationMs = 220;
    constexpr UINT CloseAnimationMs = 160;

    constexpr wchar_t MainClassName[] = L"GpuColorLite.Main";
    constexpr wchar_t AboutClassName[] = L"GpuColorLite.About";
    constexpr wchar_t RepositoryUrl[] = L"https://github.com/Zerin-emm/GpuColorLite";
    constexpr wchar_t RepositoryDisplay[] = L"github.com/Zerin-emm/GpuColorLite";
    constexpr wchar_t VersionString[] = L"1.0.0";
}

namespace Util
{
    template <class T>
    inline void SafeRelease(T *&pointer)
    {
        if (pointer)
        {
            pointer->Release();
            pointer = nullptr;
        }
    }

    inline D2D1_RECT_F Rect(float x, float y, float w, float h, float scale)
    {
        return D2D1::RectF(x * scale, y * scale, (x + w) * scale, (y + h) * scale);
    }

    inline bool PointInRect(D2D1_RECT_F rect, POINT point)
    {
        return point.x >= rect.left && point.x <= rect.right &&
               point.y >= rect.top && point.y <= rect.bottom;
    }

    inline bool RectValid(D2D1_RECT_F rect)
    {
        return rect.right > rect.left && rect.bottom > rect.top;
    }
}

// ============================================================================
//  2. Theme —— 主题色板
// ============================================================================

namespace Theme
{
    namespace
    {
        bool g_light = true;
    }

    void ReadFromSystem()
    {
        DWORD value = 1;
        DWORD size = sizeof(value);
        DWORD type = 0;
        HKEY key = nullptr;

        if (RegOpenKeyExW(HKEY_CURRENT_USER,
                          L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                          0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS)
        {
            if (RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type,
                                 reinterpret_cast<LPBYTE>(&value), &size) != ERROR_SUCCESS)
            {
                value = 1;
            }
            RegCloseKey(key);
        }

        g_light = (value != 0);
    }

    bool IsLight() { return g_light; }
    void SetLight(bool light) { g_light = light; }

    unsigned Background()     { return g_light ? 0xFFF3F3F3 : 0xFF0D0D0D; }
    unsigned TextPrimary()    { return g_light ? 0xFF1A1A1A : 0xFFE8E8E8; }
    unsigned TextSecondary()  { return g_light ? 0xFF5A5A5A : 0xFFB0B0B0; }
    unsigned InputFill()      { return g_light ? 0xFFFFFFFF : 0xFF141414; }
    unsigned InputBorder()    { return g_light ? 0xFFB8B8B8 : 0xFF4A4A4A; }
    unsigned PreviewBorder()  { return g_light ? 0xFF666666 : 0xFFB0B0B0; }
    unsigned IconNormal()     { return g_light ? 0xFF24292F : 0xFFB8B8B8; }
    unsigned HoverFill()      { return g_light ? 0x14000000 : 0x22FFFFFF; }
    unsigned PinActiveFill()  { return g_light ? 0x1A000000 : 0x26FFFFFF; }
    unsigned Accent()         { return g_light ? 0xFF0078D4 : 0xFF4CC2FF; }
    unsigned StatusOk()       { return g_light ? 0xFF107C41 : 0xFF6FCF97; }
    unsigned StatusWarn()     { return g_light ? 0xFF9D5D00 : 0xFFFFC56B; }
    unsigned StatusError()    { return g_light ? 0xFFC42B1C : 0xFFFF8A80; }

    // 统一应用窗口外观：无边框 + 圆角 + 深色标题（若有）
    void ApplyWindowChrome(HWND hwnd)
    {
        if (!hwnd)
        {
            return;
        }

        // 圆角（Windows 11）
        const int cornerPreference = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/,
                              &cornerPreference, sizeof(cornerPreference));

        // 深色模式
        BOOL dark = g_light ? FALSE : TRUE;
        DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/,
                              &dark, sizeof(dark));

        // 禁止 DWM 渲染非客户区，避免出现系统边框
        const DWORD ncDisabled = 1; // DWMNCRP_DISABLED
        DwmSetWindowAttribute(hwnd, 2 /*DWMWA_NCRENDERING_POLICY*/,
                              &ncDisabled, sizeof(ncDisabled));

        // 边框色设为“无”
        const DWORD noBorder = 0xFFFFFFFE; // DWMWA_COLOR_NONE
        DwmSetWindowAttribute(hwnd, 34 /*DWMWA_BORDER_COLOR*/,
                              &noBorder, sizeof(noBorder));
    }
}

// ============================================================================
//  3. ColorUtil —— HSV/RGB 转换
// ============================================================================

namespace ColorUtil
{
    void HsvToRgb(float h, float s, float v, int &r, int &g, int &b)
    {
        h = fmodf(h, 360.0f);
        if (h < 0.0f)
        {
            h += 360.0f;
        }

        const float c = v * s;
        const float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
        const float m = v - c;

        float rf = 0.0f, gf = 0.0f, bf = 0.0f;
        if (h < 60.0f)       { rf = c; gf = x; }
        else if (h < 120.0f) { rf = x; gf = c; }
        else if (h < 180.0f) { gf = c; bf = x; }
        else if (h < 240.0f) { gf = x; bf = c; }
        else if (h < 300.0f) { rf = x; bf = c; }
        else                 { rf = c; bf = x; }

        r = static_cast<int>((rf + m) * 255.0f + 0.5f);
        g = static_cast<int>((gf + m) * 255.0f + 0.5f);
        b = static_cast<int>((bf + m) * 255.0f + 0.5f);
    }

    void RgbToHsv(int r, int g, int b, float &h, float &s, float &v)
    {
        const float rf = r / 255.0f;
        const float gf = g / 255.0f;
        const float bf = b / 255.0f;

        const float maxValue = std::max({ rf, gf, bf });
        const float minValue = std::min({ rf, gf, bf });
        const float delta = maxValue - minValue;

        v = maxValue;
        s = (maxValue <= 0.0f) ? 0.0f : (delta / maxValue);

        if (delta <= 0.0f)
        {
            h = 0.0f;
        }
        else if (maxValue == rf)
        {
            h = 60.0f * fmodf((gf - bf) / delta, 6.0f);
        }
        else if (maxValue == gf)
        {
            h = 60.0f * (((bf - rf) / delta) + 2.0f);
        }
        else
        {
            h = 60.0f * (((rf - gf) / delta) + 4.0f);
        }

        if (h < 0.0f)
        {
            h += 360.0f;
        }
    }

    D2D1_COLOR_F FromArgb(unsigned argb)
    {
        return D2D1::ColorF(((argb >> 16) & 0xFF) / 255.0f,
                            ((argb >> 8) & 0xFF) / 255.0f,
                            (argb & 0xFF) / 255.0f,
                            ((argb >> 24) & 0xFF) / 255.0f);
    }

    unsigned ToArgb(int r, int g, int b, unsigned alpha = 0xFF)
    {
        return (alpha << 24) |
               (static_cast<unsigned>(r) << 16) |
               (static_cast<unsigned>(g) << 8) |
               static_cast<unsigned>(b);
    }
}

// ============================================================================
//  4. Draw —— D2D 绘制辅助
// ============================================================================

namespace Draw
{
    namespace
    {
        IDWriteFactory *g_writeFactory = nullptr;
        ID2D1PathGeometry *g_githubMark = nullptr;

        // GitHub 官方 SVG 路径（viewBox 0 0 32 32，fill-rule=evenodd）
        // 仅含 M / C / V / Z 指令，无椭圆弧
        constexpr wchar_t GithubPath[] =
            L"M16 0C7.16 0 0 7.16 0 16C0 23.08 4.58 29.06 10.94 31.18"
            L"C11.74 31.32 12.04 30.84 12.04 30.42C12.04 30.04 12.02 28.78 12.02 27.44"
            L"C8 28.18 6.96 26.46 6.64 25.56C6.46 25.1 5.68 23.68 5 23.3"
            L"C4.44 23 3.64 22.26 4.98 22.24C6.24 22.22 7.14 23.4 7.44 23.88"
            L"C8.88 26.3 11.18 25.62 12.1 25.2C12.24 24.16 12.66 23.46 13.12 23.06"
            L"C9.56 22.66 5.84 21.28 5.84 15.16C5.84 13.42 6.46 11.98 7.48 10.86"
            L"C7.32 10.46 6.76 8.82 7.64 6.62C7.64 6.62 8.98 6.2 12.04 8.26"
            L"C13.32 7.9 14.68 7.72 16.04 7.72C17.4 7.72 18.76 7.9 20.04 8.26"
            L"C23.1 6.18 24.44 6.62 24.44 6.62C25.32 8.82 24.76 10.46 24.6 10.86"
            L"C25.62 11.98 26.24 13.4 26.24 15.16C26.24 21.3 22.5 22.66 18.94 23.06"
            L"C19.52 23.56 20.02 24.52 20.02 26.02C20.02 28.16 20 29.88 20 30.42"
            L"C20 30.84 20.3 31.34 21.1 31.18C27.42 29.06 32 23.06 32 16"
            L"C32 7.16 24.84 0 16 0V0Z";
    }

    IDWriteFactory *EnsureWriteFactory()
    {
        if (!g_writeFactory)
        {
            DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                reinterpret_cast<IUnknown **>(&g_writeFactory));
        }
        return g_writeFactory;
    }

    // 把 SVG path 解析为 D2D 路径几何
    ID2D1PathGeometry *BuildSvgGeometry(ID2D1Factory *factory, const wchar_t *path)
    {
        if (!factory || !path)
        {
            return nullptr;
        }

        ID2D1PathGeometry *geometry = nullptr;
        if (FAILED(factory->CreatePathGeometry(&geometry)) || !geometry)
        {
            return nullptr;
        }

        ID2D1GeometrySink *sink = nullptr;
        if (FAILED(geometry->Open(&sink)) || !sink)
        {
            geometry->Release();
            return nullptr;
        }

        // fill-rule="evenodd" 对应 D2D1_FILL_MODE_ALTERNATE
        sink->SetFillMode(D2D1_FILL_MODE_ALTERNATE);

        const wchar_t *p = path;
        auto isCommand = [](wchar_t c) { return c && wcschr(L"MmLlHhVvCcSsQqTtAaZz", c) != nullptr; };
        auto skipSeparators = [&]() { while (*p && (iswspace(*p) || *p == L',')) ++p; };
        auto readNumber = [&](float &value) -> bool
        {
            skipSeparators();
            if (!*p || isCommand(*p))
            {
                return false;
            }
            wchar_t *end = nullptr;
            value = wcstof(p, &end);
            if (end == p)
            {
                return false;
            }
            p = end;
            return true;
        };

        float currentX = 0.0f, currentY = 0.0f;
        float startX = 0.0f, startY = 0.0f;
        bool figureOpen = false;
        wchar_t command = 0;

        while (true)
        {
            skipSeparators();
            if (!*p)
            {
                break;
            }
            if (isCommand(*p))
            {
                command = *p++;
            }
            else if (!command)
            {
                break;
            }

            const bool relative = iswlower(command) != 0;
            const wchar_t upper = static_cast<wchar_t>(towupper(command));

            if (upper == L'Z')
            {
                if (figureOpen)
                {
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    figureOpen = false;
                }
                currentX = startX;
                currentY = startY;
                continue;
            }

            if (upper == L'M')
            {
                float x, y;
                if (!readNumber(x) || !readNumber(y)) break;
                if (relative) { x += currentX; y += currentY; }
                if (figureOpen)
                {
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    figureOpen = false;
                }
                sink->BeginFigure(D2D1::Point2F(x, y), D2D1_FIGURE_BEGIN_FILLED);
                figureOpen = true;
                currentX = startX = x;
                currentY = startY = y;
            }
            else if (upper == L'C')
            {
                float x1, y1, x2, y2, x, y;
                if (!readNumber(x1) || !readNumber(y1) ||
                    !readNumber(x2) || !readNumber(y2) ||
                    !readNumber(x) || !readNumber(y)) break;
                if (relative)
                {
                    x1 += currentX; y1 += currentY;
                    x2 += currentX; y2 += currentY;
                    x += currentX; y += currentY;
                }
                sink->AddBezier(D2D1::BezierSegment(
                    D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), D2D1::Point2F(x, y)));
                currentX = x;
                currentY = y;
            }
            else if (upper == L'V')
            {
                float y;
                if (!readNumber(y)) break;
                if (relative) y += currentY;
                sink->AddLine(D2D1::Point2F(currentX, y));
                currentY = y;
            }
            else if (upper == L'H')
            {
                float x;
                if (!readNumber(x)) break;
                if (relative) x += currentX;
                sink->AddLine(D2D1::Point2F(x, currentY));
                currentX = x;
            }
            else
            {
                break; // 未支持的命令
            }
        }

        // Close 前必须结束所有 figure，否则 sink->Close() 会失败并污染渲染状态
        if (figureOpen)
        {
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            figureOpen = false;
        }

        const HRESULT closeResult = sink->Close();
        sink->Release();

        if (FAILED(closeResult))
        {
            geometry->Release();
            return nullptr;
        }

        return geometry;
    }

    // 必须在 BeginDraw 之前调用：预建 DWrite 工厂与几何资源
    void Prepare(ID2D1Factory *factory)
    {
        EnsureWriteFactory();

        if (g_githubMark || !factory)
        {
            return;
        }

        g_githubMark = BuildSvgGeometry(factory, GithubPath);
    }

    IDWriteTextFormat *CreateFormat(float size, float scale, DWRITE_TEXT_ALIGNMENT align,
                                    const wchar_t *family)
    {
        IDWriteFactory *factory = EnsureWriteFactory();
        if (!factory)
        {
            return nullptr;
        }

        IDWriteTextFormat *format = nullptr;
        factory->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                  DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                  size * scale, L"zh-cn", &format);
        if (format)
        {
            format->SetTextAlignment(align);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        return format;
    }

    void FillRect(ID2D1RenderTarget *target, D2D1_RECT_F rect, D2D1_COLOR_F color,
                  float scale, float radius = 0.0f)
    {
        if (!target || !Util::RectValid(rect))
        {
            return;
        }

        ID2D1SolidColorBrush *brush = nullptr;
        target->CreateSolidColorBrush(color, &brush);
        if (!brush)
        {
            return;
        }

        if (radius > 0.0f)
        {
            target->FillRoundedRectangle(
                D2D1::RoundedRect(rect, radius * scale, radius * scale), brush);
        }
        else
        {
            target->FillRectangle(rect, brush);
        }
        brush->Release();
    }

    void StrokeRect(ID2D1RenderTarget *target, D2D1_RECT_F rect, D2D1_COLOR_F color,
                    float scale, float width = 1.0f, float radius = 0.0f)
    {
        if (!target || !Util::RectValid(rect))
        {
            return;
        }

        ID2D1SolidColorBrush *brush = nullptr;
        target->CreateSolidColorBrush(color, &brush);
        if (!brush)
        {
            return;
        }

        if (radius > 0.0f)
        {
            target->DrawRoundedRectangle(
                D2D1::RoundedRect(rect, radius * scale, radius * scale), brush, width * scale);
        }
        else
        {
            target->DrawRectangle(rect, brush, width * scale);
        }
        brush->Release();
    }

    void Line(ID2D1RenderTarget *target, float x1, float y1, float x2, float y2,
              D2D1_COLOR_F color, float scale, float width)
    {
        if (!target)
        {
            return;
        }

        ID2D1SolidColorBrush *brush = nullptr;
        target->CreateSolidColorBrush(color, &brush);
        if (!brush)
        {
            return;
        }

        target->DrawLine(D2D1::Point2F(x1 * scale, y1 * scale),
                         D2D1::Point2F(x2 * scale, y2 * scale),
                         brush, width * scale);
        brush->Release();
    }

    void EllipseRing(ID2D1RenderTarget *target, float cx, float cy, float radius,
                     D2D1_COLOR_F color, float scale, float width)
    {
        if (!target)
        {
            return;
        }

        ID2D1SolidColorBrush *brush = nullptr;
        target->CreateSolidColorBrush(color, &brush);
        if (!brush)
        {
            return;
        }

        target->DrawEllipse(
            D2D1::Ellipse(D2D1::Point2F(cx * scale, cy * scale),
                          radius * scale, radius * scale),
            brush, width * scale);
        brush->Release();
    }

    // 测量文本实际渲染宽度（返回逻辑单位，已除以 scale）
    float MeasureTextWidth(const wchar_t *text, float size, float scale,
                           const wchar_t *family = L"Segoe UI Variable Text")
    {
        if (!text || !*text)
        {
            return 0.0f;
        }

        IDWriteFactory *factory = EnsureWriteFactory();
        if (!factory)
        {
            return 0.0f;
        }

        IDWriteTextFormat *format = CreateFormat(size, scale, DWRITE_TEXT_ALIGNMENT_LEADING, family);
        if (!format)
        {
            return 0.0f;
        }

        IDWriteTextLayout *layout = nullptr;
        factory->CreateTextLayout(text, static_cast<UINT32>(wcslen(text)), format,
                                  8192.0f, 512.0f, &layout);
        format->Release();

        float widthDevice = 0.0f;
        if (layout)
        {
            DWRITE_TEXT_METRICS metrics = {};
            if (SUCCEEDED(layout->GetMetrics(&metrics)))
            {
                widthDevice = metrics.widthIncludingTrailingWhitespace;
            }
            layout->Release();
        }

        return (scale > 0.0f) ? (widthDevice / scale) : widthDevice;
    }

    void Text(ID2D1RenderTarget *target, const wchar_t *text, D2D1_RECT_F rect,
              D2D1_COLOR_F color, float size, float scale,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING)
    {
        if (!target || !text || !*text || !Util::RectValid(rect))
        {
            return;
        }

        IDWriteTextFormat *format = CreateFormat(size, scale, align, L"Segoe UI Variable Text");
        if (!format)
        {
            return;
        }

        ID2D1SolidColorBrush *brush = nullptr;
        target->CreateSolidColorBrush(color, &brush);
        if (brush)
        {
            target->DrawTextW(text, static_cast<UINT32>(wcslen(text)), format, rect, brush);
            brush->Release();
        }
        format->Release();
    }

    void Glyph(ID2D1RenderTarget *target, unsigned codepoint, D2D1_RECT_F rect,
               D2D1_COLOR_F color, float size, float scale)
    {
        if (!target || !Util::RectValid(rect))
        {
            return;
        }

        IDWriteTextFormat *format = CreateFormat(size, scale, DWRITE_TEXT_ALIGNMENT_CENTER,
                                                 L"Segoe Fluent Icons");
        if (!format)
        {
            return;
        }

        ID2D1SolidColorBrush *brush = nullptr;
        target->CreateSolidColorBrush(color, &brush);
        if (brush)
        {
            const wchar_t glyph[2] = { static_cast<wchar_t>(codepoint), 0 };
            target->DrawTextW(glyph, 1, format, rect, brush);
            brush->Release();
        }
        format->Release();
    }

    // GitHub 官方图标（SVG 路径，viewBox 32x32）
    void GithubMark(ID2D1RenderTarget *target, float centerX, float centerY,
                    float size, D2D1_COLOR_F color, float scale)
    {
        if (!target || !g_githubMark || size <= 0.0f)
        {
            return;
        }

        D2D1::Matrix3x2F savedTransform;
        target->GetTransform(&savedTransform);

        // 路径坐标基于 32x32 视口
        const float unit = size / 32.0f;
        const D2D1::Matrix3x2F local =
            D2D1::Matrix3x2F::Scale(unit, unit) *
            D2D1::Matrix3x2F::Translation(centerX - size / 2.0f, centerY - size / 2.0f);
        target->SetTransform(local * savedTransform);

        ID2D1SolidColorBrush *brush = nullptr;
        target->CreateSolidColorBrush(color, &brush);
        if (brush)
        {
            target->FillGeometry(g_githubMark, brush);
            brush->Release();
        }

        target->SetTransform(savedTransform);
    }
}

// ============================================================================
//  5. UrlLauncher —— 系统 Launcher 打开链接
// ============================================================================

namespace UrlLauncher
{
    void Open(const wchar_t *url)
    {
        if (!url)
        {
            return;
        }

        // 优先用系统 Launcher：由代理进程以普通权限打开，提权运行也不会触发浏览器提权对话框
        try
        {
            winrt::Windows::System::Launcher::LaunchUriAsync(
                winrt::Windows::Foundation::Uri(winrt::hstring(url)));
            return;
        }
        catch (...)
        {
        }

        ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
    }
}

// ============================================================================
//  6. GpuColor —— NVAPI 后端 + 注册表
// ============================================================================

namespace GpuColor
{
    struct Result
    {
        bool Success = false;
        std::wstring Error;
    };

    namespace
    {
        using QueryInterfaceFn = void *(__cdecl *)(std::uint32_t id);
        using InitializeFn = int(__cdecl *)();
        using EnumPhysicalGpusFn = int(__cdecl *)(void **handles, std::uint32_t *count);
        using I2cWriteFn = int(__cdecl *)(void *gpu, void *info);
        using I2cWriteExFn = int(__cdecl *)(void *gpu, void *info, std::uint32_t *size);

#pragma pack(push, 8)
        struct NvI2cInfo
        {
            std::uint32_t version;
            std::uint32_t displayMask;
            std::uint8_t isDdcPort;
            std::uint8_t deviceAddress;
            std::uint16_t padding0;
            std::uint32_t padding1;
            std::uint8_t *registerAddress;
            std::uint32_t registerAddressSize;
            std::uint32_t padding2;
            std::uint8_t *data;
            std::uint32_t dataSize;
            std::uint32_t speed;
            std::uint32_t speedKhz;
            std::uint8_t portId;
            std::uint8_t padding3[3];
            std::uint32_t isPortIdSet;
        };
#pragma pack(pop)

        static_assert(sizeof(NvI2cInfo) == 64, "NvI2cInfo layout mismatch");
        static_assert(offsetof(NvI2cInfo, deviceAddress) == 9, "NvI2cInfo offset mismatch");
        static_assert(offsetof(NvI2cInfo, registerAddress) == 16, "NvI2cInfo offset mismatch");
        static_assert(offsetof(NvI2cInfo, data) == 32, "NvI2cInfo offset mismatch");
        static_assert(offsetof(NvI2cInfo, dataSize) == 40, "NvI2cInfo offset mismatch");
        static_assert(offsetof(NvI2cInfo, speedKhz) == 48, "NvI2cInfo offset mismatch");
        static_assert(offsetof(NvI2cInfo, isPortIdSet) == 56, "NvI2cInfo offset mismatch");

        constexpr std::uint32_t IdInitialize = 0x0150E828;
        constexpr std::uint32_t IdEnumPhysicalGpus = 0xE5AC921F;
        constexpr std::uint32_t IdI2cWrite = 0xE812EB07;
        constexpr std::uint32_t IdI2cWriteEx = 0x283AC65A;

        constexpr wchar_t RegistrySubKey[] = L"Software\\GpuColorLite";
        constexpr wchar_t RegistryColor[] = L"Color";
        constexpr wchar_t RegistryHue[] = L"HSV_Hue";
        constexpr wchar_t RegistrySaturation[] = L"HSV_Sat";
        constexpr wchar_t RegistryValue[] = L"HSV_Val";

        struct LibraryGuard
        {
            HMODULE Handle = nullptr;
            ~LibraryGuard()
            {
                if (Handle)
                {
                    FreeLibrary(Handle);
                }
            }
        };

        std::wstring LogPath()
        {
            wchar_t buffer[MAX_PATH * 2] = {};
            const DWORD length = GetModuleFileNameW(nullptr, buffer,
                                                    static_cast<DWORD>(std::size(buffer)));
            if (length == 0 || length >= std::size(buffer))
            {
                return L"log.txt";
            }

            std::wstring path(buffer, length);
            const size_t slash = path.find_last_of(L'\\');
            if (slash == std::wstring::npos)
            {
                return L"log.txt";
            }
            return path.substr(0, slash + 1) + L"log.txt";
        }

        std::wstring StatusError(const wchar_t *operation, int status)
        {
            return std::wstring(operation) + L" 失败，NVAPI 状态 = " + std::to_wstring(status);
        }
    }

    void Log(const std::wstring &message) noexcept
    {
        try
        {
            FILE *file = nullptr;
            if (_wfopen_s(&file, LogPath().c_str(), L"a") == 0 && file)
            {
                SYSTEMTIME now;
                GetLocalTime(&now);
                fwprintf(file, L"%04d-%02d-%02d %02d:%02d:%02d  %ls\n",
                         now.wYear, now.wMonth, now.wDay,
                         now.wHour, now.wMinute, now.wSecond, message.c_str());
                fclose(file);
            }
        }
        catch (...)
        {
        }
    }

    Result SetColor(unsigned char r, unsigned char g, unsigned char b) noexcept
    {
        try
        {
            LibraryGuard library;
            library.Handle = LoadLibraryW(L"nvapi64.dll");
            if (!library.Handle)
            {
                return { false, L"未找到 nvapi64.dll（需要 NVIDIA 显卡驱动）" };
            }

            const auto query = reinterpret_cast<QueryInterfaceFn>(
                GetProcAddress(library.Handle, "nvapi_QueryInterface"));
            if (!query)
            {
                return { false, L"未找到 nvapi_QueryInterface" };
            }

            const auto initialize = reinterpret_cast<InitializeFn>(query(IdInitialize));
            const auto enumGpus = reinterpret_cast<EnumPhysicalGpusFn>(query(IdEnumPhysicalGpus));
            const auto write = reinterpret_cast<I2cWriteFn>(query(IdI2cWrite));
            const auto writeEx = reinterpret_cast<I2cWriteExFn>(query(IdI2cWriteEx));

            if (!initialize || !enumGpus || !write || !writeEx)
            {
                return { false, L"缺少必要的 NVAPI 接口" };
            }

            int status = initialize();
            if (status != 0)
            {
                return { false, StatusError(L"NvAPI_Initialize", status) };
            }

            void *gpus[64] = {};
            std::uint32_t gpuCount = 0;
            status = enumGpus(gpus, &gpuCount);
            if (status != 0)
            {
                return { false, StatusError(L"NvAPI_EnumPhysicalGPUs", status) };
            }
            if (gpuCount == 0)
            {
                return { false, L"未检测到 NVIDIA GPU" };
            }
            if (gpuCount > std::size(gpus))
            {
                gpuCount = static_cast<std::uint32_t>(std::size(gpus));
            }

            // 灯效控制报文：0xAA 0xEF 0x12 0x03 0x01 0xFF R G B + 16 位校验
            std::uint8_t packet[11] = { 0xAA, 0xEF, 0x12, 0x03, 0x01, 0xFF, r, g, b, 0, 0 };
            std::uint16_t checksum = 0;
            for (size_t i = 0; i < 9; ++i)
            {
                checksum = static_cast<std::uint16_t>(checksum + packet[i]);
            }
            packet[9] = static_cast<std::uint8_t>(checksum & 0xFF);
            packet[10] = static_cast<std::uint8_t>(checksum >> 8);

            std::uint8_t registerAddress = 0;
            bool succeeded = false;
            int lastStatus = 0;

            for (std::uint32_t i = 0; i < gpuCount; ++i)
            {
                NvI2cInfo info = {};
                info.version = 0x30040;
                info.deviceAddress = 0xC2;
                info.registerAddress = &registerAddress;
                info.registerAddressSize = 0;
                info.data = packet;
                info.dataSize = static_cast<std::uint32_t>(sizeof(packet));
                info.speed = 0xFFFF;
                info.speedKhz = 6;
                info.portId = 1;
                info.isPortIdSet = 1;

                status = write(gpus[i], &info);
                if (status == -104 || status == -5)
                {
                    std::uint32_t size = 0;
                    status = writeEx(gpus[i], &info, &size);
                }

                lastStatus = status;
                succeeded = succeeded || (status == 0);
            }

            if (!succeeded)
            {
                return { false, StatusError(L"NVAPI I2C 写入", lastStatus) +
                                L"。请先关闭 iGame Center / iGC.Lite 等占用工具" };
            }

            Log(L"已应用 RGB(" + std::to_wstring(r) + L", " +
                std::to_wstring(g) + L", " + std::to_wstring(b) + L")");
            return { true, {} };
        }
        catch (const std::exception &error)
        {
            const size_t length = std::strlen(error.what());
            return { false, L"后端异常: " +
                            std::wstring(error.what(), error.what() + length) };
        }
        catch (...)
        {
            return { false, L"后端未知异常" };
        }
    }

    bool LoadColor(unsigned char &r, unsigned char &g, unsigned char &b) noexcept
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, RegistrySubKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        {
            return false;
        }

        DWORD value = 0;
        DWORD size = sizeof(value);
        DWORD type = 0;
        const LONG status = RegQueryValueExW(key, RegistryColor, nullptr, &type,
                                             reinterpret_cast<LPBYTE>(&value), &size);
        RegCloseKey(key);

        if (status != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(value))
        {
            return false;
        }

        r = static_cast<unsigned char>((value >> 16) & 0xFF);
        g = static_cast<unsigned char>((value >> 8) & 0xFF);
        b = static_cast<unsigned char>(value & 0xFF);
        return true;
    }

    void SaveColor(unsigned char r, unsigned char g, unsigned char b) noexcept
    {
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, RegistrySubKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                            KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        {
            return;
        }

        const DWORD value = (static_cast<DWORD>(r) << 16) |
                            (static_cast<DWORD>(g) << 8) |
                            static_cast<DWORD>(b);
        RegSetValueExW(key, RegistryColor, 0, REG_DWORD,
                       reinterpret_cast<const BYTE *>(&value), sizeof(value));
        RegCloseKey(key);
    }

    bool SaveHsv(unsigned hue, unsigned saturationPermille, unsigned valuePermille) noexcept
    {
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, RegistrySubKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                            KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        {
            return false;
        }

        const LONG r1 = RegSetValueExW(key, RegistryHue, 0, REG_DWORD,
                                       reinterpret_cast<const BYTE *>(&hue), sizeof(hue));
        const LONG r2 = RegSetValueExW(key, RegistrySaturation, 0, REG_DWORD,
                                       reinterpret_cast<const BYTE *>(&saturationPermille),
                                       sizeof(saturationPermille));
        const LONG r3 = RegSetValueExW(key, RegistryValue, 0, REG_DWORD,
                                       reinterpret_cast<const BYTE *>(&valuePermille),
                                       sizeof(valuePermille));
        RegCloseKey(key);

        return r1 == ERROR_SUCCESS && r2 == ERROR_SUCCESS && r3 == ERROR_SUCCESS;
    }

    bool LoadHsv(unsigned &hue, unsigned &saturationPermille, unsigned &valuePermille) noexcept
    {
        hue = 0xFFFFFFFFu;
        saturationPermille = 0;
        valuePermille = 0;

        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, RegistrySubKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        {
            return false;
        }

        DWORD type = 0;
        DWORD size = sizeof(DWORD);
        const LONG status = RegQueryValueExW(key, RegistryHue, nullptr, &type,
                                             reinterpret_cast<LPBYTE>(&hue), &size);
        if (status == ERROR_SUCCESS && type == REG_DWORD)
        {
            size = sizeof(DWORD);
            RegQueryValueExW(key, RegistrySaturation, nullptr, &type,
                             reinterpret_cast<LPBYTE>(&saturationPermille), &size);
            size = sizeof(DWORD);
            RegQueryValueExW(key, RegistryValue, nullptr, &type,
                             reinterpret_cast<LPBYTE>(&valuePermille), &size);
        }
        else
        {
            hue = 0xFFFFFFFFu;
        }

        RegCloseKey(key);

        if (hue != 0xFFFFFFFFu)
        {
            if (hue >= 360) hue = 0;
            if (saturationPermille > 1000) saturationPermille = 1000;
            if (valuePermille > 1000) valuePermille = 1000;
            return true;
        }
        return false;
    }
}

// ============================================================================
//  7. AboutWindow —— 关于窗口
// ============================================================================

namespace AboutWindow
{
    namespace
    {
        HWND g_window = nullptr;
        ID2D1Factory *g_factory = nullptr;
        ID2D1HwndRenderTarget *g_target = nullptr;
        float g_scale = 1.0f;

        POINT g_mouse = { -9999, -9999 };
        bool g_tracking = false;
        bool g_hoverClose = false;
        bool g_hoverOk = false;
        bool g_hoverLink = false;
        int g_pressed = 0; // 1=关闭 2=确定 3=链接

        D2D1_RECT_F CloseRect()  { return Util::Rect(Constant::AboutWidth - 44.0f, 4.0f, 40.0f, 28.0f, g_scale); }
        D2D1_RECT_F OkRect()     { return Util::Rect(Constant::AboutWidth / 2 - 60.0f, Constant::AboutHeight - 52.0f, 120.0f, 36.0f, g_scale); }
        D2D1_RECT_F LinkRect()   { return Util::Rect(0.0f, 200.0f, Constant::AboutWidth, 24.0f, g_scale); }

        void DrawContents(ID2D1RenderTarget *target)
        {
            const float width = Constant::AboutWidth;
            const D2D1_COLOR_F text = ColorUtil::FromArgb(Theme::TextPrimary());
            const D2D1_COLOR_F textSecondary = ColorUtil::FromArgb(Theme::TextSecondary());
            const D2D1_COLOR_F accent = ColorUtil::FromArgb(Theme::Accent());
            const D2D1_COLOR_F hover = ColorUtil::FromArgb(Theme::HoverFill());
            const D2D1_COLOR_F border = ColorUtil::FromArgb(Theme::InputBorder());

            target->Clear(ColorUtil::FromArgb(Theme::Background()));

            // 关闭按钮（右上角，不参与居中布局）
            {
                const D2D1_RECT_F rect = CloseRect();
                if (g_hoverClose || g_pressed == 1)
                {
                    const unsigned fill = (g_pressed == 1) ? 0xFFB00E1E : 0xFFE81123;
                    Draw::FillRect(target, rect, ColorUtil::FromArgb(fill), g_scale, 6.0f);
                }
                const D2D1_COLOR_F color = (g_hoverClose || g_pressed == 1)
                    ? ColorUtil::FromArgb(0xFFFFFFFF)
                    : ColorUtil::FromArgb(Theme::IconNormal());
                Draw::Glyph(target, 0xE8BB, rect, color, 12.0f, g_scale);
            }

            // 以下内容全部水平居中，左右对称
            Draw::Text(target, L"GpuColorLite", Util::Rect(0, 22, width, 34, g_scale),
                       text, 20.0f, g_scale, DWRITE_TEXT_ALIGNMENT_CENTER);

            wchar_t versionLine[64];
            swprintf_s(versionLine, L"版本 %s", Constant::VersionString);
            Draw::Text(target, versionLine, Util::Rect(0, 60, width, 22, g_scale),
                       textSecondary, 13.0f, g_scale, DWRITE_TEXT_ALIGNMENT_CENTER);

            // 分隔线：左右等距
            Draw::Line(target, 40.0f, 94.0f, width - 40.0f, 94.0f, border, g_scale, 1.0f);

            Draw::Text(target, L"NVIDIA 显卡 RGB 灯效控制工具",
                       Util::Rect(0, 104, width, 24, g_scale),
                       text, 14.5f, g_scale, DWRITE_TEXT_ALIGNMENT_CENTER);
            Draw::Text(target, L"基于 Direct2D 自绘，无外部运行时依赖",
                       Util::Rect(0, 130, width, 22, g_scale),
                       textSecondary, 12.5f, g_scale, DWRITE_TEXT_ALIGNMENT_CENTER);
            Draw::Text(target, L"NVAPI I2C 直写 · 支持深浅色跟随系统",
                       Util::Rect(0, 152, width, 22, g_scale),
                       textSecondary, 12.5f, g_scale, DWRITE_TEXT_ALIGNMENT_CENTER);

            // 兼容性提示：仅在该型号实测，其他显卡未验证
            Draw::Text(target, L"仅测试：七彩虹 RTX 5060 Ultra 侧边灯条",
                       Util::Rect(0, 176, width, 20, g_scale),
                       ColorUtil::FromArgb(Theme::StatusWarn()), 10.5f, g_scale,
                       DWRITE_TEXT_ALIGNMENT_CENTER);

            // 仓库链接（居中 + 居中下划线）
            {
                const D2D1_RECT_F rect = LinkRect();
                if (g_hoverLink)
                {
                    Draw::FillRect(target,
                                   D2D1::RectF(rect.left - 8 * g_scale, rect.top,
                                               rect.right + 8 * g_scale, rect.bottom),
                                   hover, g_scale, 4.0f);
                }
                Draw::Text(target, Constant::RepositoryDisplay, rect, accent, 12.5f, g_scale,
                           DWRITE_TEXT_ALIGNMENT_CENTER);

                // 下划线宽度按文本实际渲染宽度计算，精确贴合
                const float centerX = width / 2.0f;
                const float underlineY = (rect.top + rect.bottom) / 2.0f / g_scale + 9.0f;
                const float textWidth = Draw::MeasureTextWidth(Constant::RepositoryDisplay,
                                                               12.5f, g_scale);
                const float halfWidth = textWidth / 2.0f;
                Draw::Line(target, centerX - halfWidth, underlineY,
                           centerX + halfWidth, underlineY, accent, g_scale, 1.0f);
            }

            // 确定按钮（居中）
            {
                const D2D1_RECT_F rect = OkRect();
                const unsigned fill = (g_pressed == 2) ? Theme::PinActiveFill()
                                    : (g_hoverOk ? Theme::HoverFill() : Theme::InputFill());
                Draw::FillRect(target, rect, ColorUtil::FromArgb(fill), g_scale, 6.0f);
                Draw::StrokeRect(target, rect, border, g_scale, 1.0f, 6.0f);
                Draw::Text(target, L"确定", rect, text, 14.0f, g_scale,
                           DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        }

        void UpdateHover()
        {
            g_hoverClose = Util::PointInRect(CloseRect(), g_mouse);
            g_hoverOk = Util::PointInRect(OkRect(), g_mouse);
            g_hoverLink = Util::PointInRect(LinkRect(), g_mouse);
        }

        void Paint(HWND hwnd)
        {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);

            if (!g_target)
            {
                RECT rc;
                GetClientRect(hwnd, &rc);
                if (g_factory)
                {
                    g_factory->CreateHwndRenderTarget(
                        D2D1::RenderTargetProperties(),
                        D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right, rc.bottom)),
                        &g_target);
                }
            }

            if (g_target)
            {
                g_target->BeginDraw();
                DrawContents(g_target);
                if (g_target->EndDraw() == D2DERR_RECREATE_TARGET)
                {
                    Util::SafeRelease(g_target);
                }
            }

            EndPaint(hwnd, &ps);
        }

        LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
        {
            switch (msg)
            {
            case WM_ACTIVATE:
                Theme::ApplyWindowChrome(hwnd);
                break;

            case WM_NCACTIVATE:
                // 拦截非客户区激活重绘：否则失焦时 DWM 会重新画出系统标题栏
                Theme::ApplyWindowChrome(hwnd);
                return TRUE;

            case WM_MOUSEMOVE:
                if (!g_tracking)
                {
                    TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd, 0 };
                    TrackMouseEvent(&track);
                    g_tracking = true;
                }
                g_mouse.x = GET_X_LPARAM(lParam);
                g_mouse.y = GET_Y_LPARAM(lParam);
                UpdateHover();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;

            case WM_MOUSELEAVE:
                g_tracking = false;
                g_mouse.x = g_mouse.y = -9999;
                UpdateHover();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;

            case WM_LBUTTONDOWN:
            {
                POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                g_mouse = point;

                if (Util::PointInRect(CloseRect(), point))      g_pressed = 1;
                else if (Util::PointInRect(OkRect(), point))    g_pressed = 2;
                else if (Util::PointInRect(LinkRect(), point))  g_pressed = 3;

                if (g_pressed)
                {
                    SetCapture(hwnd);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            case WM_LBUTTONUP:
            {
                POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                const int pressed = g_pressed;
                g_pressed = 0;

                if (pressed)
                {
                    ReleaseCapture();
                }

                if (pressed == 1 && Util::PointInRect(CloseRect(), point))
                {
                    DestroyWindow(hwnd);
                    return 0;
                }
                if (pressed == 2 && Util::PointInRect(OkRect(), point))
                {
                    DestroyWindow(hwnd);
                    return 0;
                }
                if (pressed == 3 && Util::PointInRect(LinkRect(), point))
                {
                    UrlLauncher::Open(Constant::RepositoryUrl);
                }

                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            case WM_KEYDOWN:
                if (wParam == VK_ESCAPE || wParam == VK_RETURN)
                {
                    DestroyWindow(hwnd);
                    return 0;
                }
                break;

            case WM_NCHITTEST:
            {
                LRESULT hit = DefWindowProcW(hwnd, msg, wParam, lParam);
                if (hit == HTCLIENT)
                {
                    POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                    ScreenToClient(hwnd, &point);
                    if (point.y < static_cast<int>(36 * g_scale) &&
                        !Util::PointInRect(CloseRect(), point))
                    {
                        return HTCAPTION;
                    }
                }
                return hit;
            }

            case WM_SETCURSOR:
                if (LOWORD(lParam) == HTCLIENT)
                {
                    POINT point;
                    GetCursorPos(&point);
                    ScreenToClient(hwnd, &point);
                    const bool clickable = Util::PointInRect(CloseRect(), point) ||
                                           Util::PointInRect(OkRect(), point) ||
                                           Util::PointInRect(LinkRect(), point);
                    SetCursor(LoadCursorW(nullptr, clickable ? IDC_HAND : IDC_ARROW));
                    return TRUE;
                }
                break;

            case WM_ERASEBKGND:
                return 1;

            case WM_SIZE:
                if (g_target)
                {
                    g_target->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
                }
                return 0;

            case WM_PAINT:
                Paint(hwnd);
                return 0;

            case WM_CLOSE:
                DestroyWindow(hwnd);
                return 0;

            case WM_DESTROY:
                Util::SafeRelease(g_target);
                g_window = nullptr;
                return 0;
            }

            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    void Show(HWND owner)
    {
        if (g_window && IsWindow(g_window))
        {
            SetForegroundWindow(g_window);
            return;
        }

        if (!g_factory)
        {
            D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_factory);
        }

        static bool registered = false;
        if (!registered)
        {
            WNDCLASSEXW wc = { sizeof(wc) };
            wc.lpfnWndProc = WindowProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = Constant::AboutClassName;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = nullptr;
            RegisterClassExW(&wc);
            registered = true;
        }

        const UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
        g_scale = (dpi > 0) ? (dpi / 96.0f) : 1.0f;

        const int width = static_cast<int>(Constant::AboutWidth * g_scale);
        const int height = static_cast<int>(Constant::AboutHeight * g_scale);

        int x = 0;
        int y = 0;
        if (owner)
        {
            RECT ownerRect;
            GetWindowRect(owner, &ownerRect);
            x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
            y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;
        }

        // 无 WS_CAPTION：DWM 不会绘制标题栏；WS_THICKFRAME 保留阴影与圆角
        // 纯 WS_POPUP：没有非客户区，因此不会出现系统边框或标题栏
        const DWORD style = WS_POPUP;

        g_window = CreateWindowExW(WS_EX_TOOLWINDOW, Constant::AboutClassName,
                                   L"关于 GpuColorLite", style,
                                   x, y, width, height,
                                   owner, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!g_window)
        {
            return;
        }

        Theme::ApplyWindowChrome(g_window);

        ShowWindow(g_window, SW_SHOW);
        UpdateWindow(g_window);
        SetForegroundWindow(g_window);
    }

    void Close()
    {
        if (g_window && IsWindow(g_window))
        {
            DestroyWindow(g_window);
        }
    }

    bool IsOpen()
    {
        return g_window && IsWindow(g_window);
    }

    void OnSystemThemeChanged()
    {
        if (g_window && IsWindow(g_window))
        {
            Theme::ApplyWindowChrome(g_window);
            InvalidateRect(g_window, nullptr, FALSE);
        }
    }
}

// ============================================================================
//  8. MainWindow —— 主取色窗口
// ============================================================================

namespace MainWindow
{
    namespace
    {
        // 按钮按下目标
        enum PressTarget
        {
            PressNone = 0,
            PressClose = 1,
            PressPin = 2,
            PressGithub = 3,
            PressAbout = 4
        };

        // 拖拽目标
        enum DragTarget
        {
            DragNone = 0,
            DragHue = 1,
            DragSquare = 2
        };

        // 输入焦点
        enum FocusField
        {
            FocusNone = -1,
            FocusRed = 0,
            FocusGreen = 1,
            FocusBlue = 2,
            FocusHex = 3
        };

        ID2D1Factory *g_factory = nullptr;
        ID2D1HwndRenderTarget *g_target = nullptr;
        IWICImagingFactory *g_wic = nullptr;
        HWND g_window = nullptr;
        float g_scale = 1.0f;
        int g_windowWidth = 0;
        int g_windowHeight = 0;
        bool g_closing = false;


        // 取色状态
        float g_hue = 0.0f;
        float g_saturation = 1.0f;
        float g_value = 1.0f;

        // 交互状态
        int g_focus = FocusNone;
        std::wstring g_editBuffer;
        int g_drag = DragNone;
        int g_press = PressNone;
        bool g_alwaysOnTop = true;
        POINT g_mouse = { -9999, -9999 };
        bool g_tracking = false;
        std::wstring g_statusText;
        unsigned g_statusColor = 0;

        // 位图缓存
        IWICBitmap *g_ringWic = nullptr;
        ID2D1Bitmap *g_ringBitmap = nullptr;
        ID2D1RenderTarget *g_ringOwner = nullptr;
        float g_ringScale = -1.0f;

        IWICBitmap *g_squareWic = nullptr;
        ID2D1Bitmap *g_squareBitmap = nullptr;
        ID2D1RenderTarget *g_squareOwner = nullptr;
        float g_squareScale = -1.0f;
        float g_squareHue = -1.0f;

        // ---- 布局 ----
        D2D1_RECT_F HexRowRect()          { return Util::Rect(452, 54, 110, 32, g_scale); }
        D2D1_RECT_F RgbRowRect(int index) { return Util::Rect(452, 114 + index * 60, 110, 32, g_scale); }
        float RgbRowCenterY(int index)    { return 130.0f + index * 60.0f; }
        D2D1_RECT_F PreviewRect()         { return Util::Rect(452, 294, 110, 32, g_scale); }

        D2D1_RECT_F SquareRect()
        {
            const float half = Constant::SquareSize / 2.0f;
            return D2D1::RectF((Constant::RingCenterX - half) * g_scale,
                               (Constant::RingCenterY - half) * g_scale,
                               (Constant::RingCenterX + half) * g_scale,
                               (Constant::RingCenterY + half) * g_scale);
        }

        // 右上角按钮（从右到左：关闭 / 置顶 / GitHub / 关于）
        D2D1_RECT_F AboutButtonRect()  { return Util::Rect(526, 6, 24, 24, g_scale); }
        D2D1_RECT_F GithubButtonRect() { return Util::Rect(554, 6, 24, 24, g_scale); }
        D2D1_RECT_F PinButtonRect()    { return Util::Rect(582, 6, 24, 24, g_scale); }
        D2D1_RECT_F CloseButtonRect()  { return Util::Rect(610, 6, 24, 24, g_scale); }

        void CurrentRgb(int &r, int &g, int &b)
        {
            ColorUtil::HsvToRgb(g_hue, g_saturation, g_value, r, g, b);
        }

        // ---- 位图构建 ----
        void BuildRingBitmap(ID2D1RenderTarget *target)
        {
            if (!g_wic || !target)
            {
                return;
            }

            if (!g_ringWic || g_ringScale != g_scale)
            {
                Util::SafeRelease(g_ringWic);

                const int dimension = static_cast<int>(2 * Constant::RingOuter * g_scale) + 4;
                const float center = dimension / 2.0f;

                g_wic->CreateBitmap(dimension, dimension, GUID_WICPixelFormat32bppPBGRA,
                                    WICBitmapCacheOnDemand, &g_ringWic);

                WICRect lockRect = { 0, 0, dimension, dimension };
                IWICBitmapLock *lock = nullptr;
                g_ringWic->Lock(&lockRect, WICBitmapLockWrite, &lock);

                UINT stride = 0;
                BYTE *data = nullptr;
                UINT capacity = 0;
                lock->GetStride(&stride);
                lock->GetDataPointer(&capacity, &data);

                const float inner = Constant::RingInner * g_scale;
                const float outer = Constant::RingOuter * g_scale;
                const float feather = 1.4f * g_scale;

                for (int y = 0; y < dimension; ++y)
                {
                    for (int x = 0; x < dimension; ++x)
                    {
                        const float dx = x - center;
                        const float dy = y - center;
                        const float radius = sqrtf(dx * dx + dy * dy);
                        BYTE *pixel = data + y * stride + x * 4;

                        float coverage = std::min((radius - (inner - feather)) / feather,
                                                  ((outer + feather) - radius) / feather);
                        coverage = std::clamp(coverage, 0.0f, 1.0f);

                        if (coverage <= 0.0f)
                        {
                            pixel[0] = pixel[1] = pixel[2] = pixel[3] = 0;
                            continue;
                        }

                        float degrees = atan2f(dy, dx) * 57.29578f;
                        if (degrees < 0.0f)
                        {
                            degrees += 360.0f;
                        }

                        int r, g, b;
                        ColorUtil::HsvToRgb(degrees, 1.0f, 1.0f, r, g, b);
                        pixel[0] = static_cast<BYTE>(b * coverage);
                        pixel[1] = static_cast<BYTE>(g * coverage);
                        pixel[2] = static_cast<BYTE>(r * coverage);
                        pixel[3] = static_cast<BYTE>(255 * coverage);
                    }
                }

                lock->Release();
                g_ringScale = g_scale;
                g_ringOwner = nullptr;
                Util::SafeRelease(g_ringBitmap);
            }

            if (!g_ringBitmap || g_ringOwner != target)
            {
                Util::SafeRelease(g_ringBitmap);
                target->CreateBitmapFromWicBitmap(g_ringWic, nullptr, &g_ringBitmap);
                g_ringOwner = target;
            }
        }

        void BuildSquareBitmap(ID2D1RenderTarget *target)
        {
            if (!g_wic || !target)
            {
                return;
            }

            if (!g_squareWic || g_squareScale != g_scale || fabsf(g_squareHue - g_hue) > 1e-4f)
            {
                Util::SafeRelease(g_squareWic);

                int dimension = static_cast<int>(Constant::SquareSize * g_scale);
                if (dimension < 2)
                {
                    dimension = 2;
                }

                g_wic->CreateBitmap(dimension, dimension, GUID_WICPixelFormat32bppPBGRA,
                                    WICBitmapCacheOnDemand, &g_squareWic);

                WICRect lockRect = { 0, 0, dimension, dimension };
                IWICBitmapLock *lock = nullptr;
                g_squareWic->Lock(&lockRect, WICBitmapLockWrite, &lock);

                UINT stride = 0;
                BYTE *data = nullptr;
                UINT capacity = 0;
                lock->GetStride(&stride);
                lock->GetDataPointer(&capacity, &data);

                for (int y = 0; y < dimension; ++y)
                {
                    for (int x = 0; x < dimension; ++x)
                    {
                        const float s = (x + 0.5f) / dimension;
                        const float v = 1.0f - (y + 0.5f) / dimension;

                        int r, g, b;
                        ColorUtil::HsvToRgb(g_hue, s, v, r, g, b);

                        BYTE *pixel = data + y * stride + x * 4;
                        pixel[0] = static_cast<BYTE>(b);
                        pixel[1] = static_cast<BYTE>(g);
                        pixel[2] = static_cast<BYTE>(r);
                        pixel[3] = 255;
                    }
                }

                lock->Release();
                g_squareScale = g_scale;
                g_squareHue = g_hue;
                g_squareOwner = nullptr;
                Util::SafeRelease(g_squareBitmap);
            }

            if (!g_squareBitmap || g_squareOwner != target)
            {
                Util::SafeRelease(g_squareBitmap);
                target->CreateBitmapFromWicBitmap(g_squareWic, nullptr, &g_squareBitmap);
                g_squareOwner = target;
            }
        }

        // ---- 业务 ----
        void ApplyColor()
        {
            int r, g, b;
            CurrentRgb(r, g, b);

            const GpuColor::Result result = GpuColor::SetColor(
                static_cast<unsigned char>(r),
                static_cast<unsigned char>(g),
                static_cast<unsigned char>(b));

            const bool saved = GpuColor::SaveHsv(
                static_cast<unsigned>(g_hue),
                static_cast<unsigned>(g_saturation * 1000.0f),
                static_cast<unsigned>(g_value * 1000.0f));

            if (result.Success && saved)
            {
                g_statusText = L"已应用并保存";
                g_statusColor = Theme::StatusOk();
            }
            else if (result.Success && !saved)
            {
                g_statusText = L"已应用到显卡，但保存失败";
                g_statusColor = Theme::StatusWarn();
            }
            else if (!result.Success && saved)
            {
                g_statusText = L"已保存，显卡应用失败: " + result.Error;
                g_statusColor = Theme::StatusWarn();
                GpuColor::Log(result.Error);
            }
            else
            {
                g_statusText = L"应用与保存均失败: " + result.Error;
                g_statusColor = Theme::StatusError();
                GpuColor::Log(result.Error);
            }
        }

        void RestoreSavedState()
        {
            unsigned hue = 0, saturation = 0, value = 0;
            if (GpuColor::LoadHsv(hue, saturation, value))
            {
                g_hue = static_cast<float>(hue);
                g_saturation = std::clamp(saturation / 1000.0f, 0.0f, 1.0f);
                g_value = std::clamp(value / 1000.0f, 0.0f, 1.0f);
                return;
            }

            unsigned char r = 0, g = 0, b = 0;
            if (GpuColor::LoadColor(r, g, b))
            {
                ColorUtil::RgbToHsv(r, g, b, g_hue, g_saturation, g_value);
            }
        }

        void ApplyDrag(POINT point)
        {
            const float mx = point.x / g_scale;
            const float my = point.y / g_scale;

            if (g_drag == DragHue)
            {
                const float dx = mx - Constant::RingCenterX;
                const float dy = my - Constant::RingCenterY;
                float degrees = atan2f(dy, dx) * 57.29578f;
                if (degrees < 0.0f)
                {
                    degrees += 360.0f;
                }
                g_hue = degrees;
            }
            else if (g_drag == DragSquare)
            {
                const float half = Constant::SquareSize / 2.0f;
                const float s = (mx - (Constant::RingCenterX - half)) / Constant::SquareSize;
                const float v = 1.0f - (my - (Constant::RingCenterY - half)) / Constant::SquareSize;
                g_saturation = std::clamp(s, 0.0f, 1.0f);
                g_value = std::clamp(v, 0.0f, 1.0f);
            }
        }

        // ---- 绘制 ----
        void DrawChromeButtons(ID2D1RenderTarget *target)
        {
            const D2D1_COLOR_F hover = ColorUtil::FromArgb(Theme::HoverFill());
            const D2D1_COLOR_F icon = ColorUtil::FromArgb(Theme::IconNormal());
            const D2D1_COLOR_F text = ColorUtil::FromArgb(Theme::TextPrimary());

            // 关于
            {
                const D2D1_RECT_F rect = AboutButtonRect();
                const bool isHover = Util::PointInRect(rect, g_mouse) && !g_press;
                const bool isPressed = (g_press == PressAbout);
                if (isHover || isPressed)
                {
                    Draw::FillRect(target, rect, hover, g_scale, 4.0f);
                }
                Draw::Glyph(target, 0xE946, rect, icon, 13.0f, g_scale);
            }

            // GitHub
            {
                const D2D1_RECT_F rect = GithubButtonRect();
                const bool isHover = Util::PointInRect(rect, g_mouse) && !g_press;
                const bool isPressed = (g_press == PressGithub);
                if (isHover || isPressed)
                {
                    Draw::FillRect(target, rect, hover, g_scale, 4.0f);
                }
                Draw::GithubMark(target,
                                 (rect.left + rect.right) / 2.0f,
                                 (rect.top + rect.bottom) / 2.0f,
                                 15.0f * g_scale, icon, g_scale);
            }

            // 置顶
            {
                const D2D1_RECT_F rect = PinButtonRect();
                const bool isHover = Util::PointInRect(rect, g_mouse) && !g_press;
                const bool isPressed = (g_press == PressPin);
                if (g_alwaysOnTop || isHover || isPressed)
                {
                    const unsigned fill = isPressed ? Theme::HoverFill()
                                        : (g_alwaysOnTop ? Theme::PinActiveFill() : Theme::HoverFill());
                    Draw::FillRect(target, rect, ColorUtil::FromArgb(fill), g_scale, 4.0f);
                }
                Draw::Glyph(target, 0xE840, rect, g_alwaysOnTop ? text : icon, 13.0f, g_scale);
            }

            // 关闭
            {
                const D2D1_RECT_F rect = CloseButtonRect();
                const bool isHover = Util::PointInRect(rect, g_mouse) && !g_press;
                const bool isPressed = (g_press == PressClose);
                if (isHover || isPressed)
                {
                    const unsigned fill = isPressed ? 0xFFB00E1E : 0xFFE81123;
                    Draw::FillRect(target, rect, ColorUtil::FromArgb(fill), g_scale, 6.0f);
                }
                const D2D1_COLOR_F color = (isHover || isPressed)
                    ? ColorUtil::FromArgb(0xFFFFFFFF)
                    : ColorUtil::FromArgb(Theme::TextSecondary());
                Draw::Glyph(target, 0xE8BB, rect, color, 12.0f, g_scale);
            }
        }

        void DrawPicker(ID2D1RenderTarget *target, int r, int g, int b)
        {
            // 色相环
            const D2D1_RECT_F ringRect = D2D1::RectF(
                (Constant::RingCenterX - Constant::RingOuter) * g_scale,
                (Constant::RingCenterY - Constant::RingOuter) * g_scale,
                (Constant::RingCenterX + Constant::RingOuter) * g_scale,
                (Constant::RingCenterY + Constant::RingOuter) * g_scale);
            if (g_ringBitmap)
            {
                target->DrawBitmap(g_ringBitmap, ringRect, 1.0f,
                                   D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }

            // 色相手柄
            {
                const float angle = g_hue * 0.0174533f;
                const float radius = (Constant::RingInner + Constant::RingOuter) / 2.0f;
                const float hx = Constant::RingCenterX + cosf(angle) * radius;
                const float hy = Constant::RingCenterY + sinf(angle) * radius;
                Draw::EllipseRing(target, hx, hy, 7.0f, ColorUtil::FromArgb(0xFFFFFFFF), g_scale, 2.2f);
                Draw::EllipseRing(target, hx, hy, 8.4f, ColorUtil::FromArgb(0x66000000), g_scale, 1.0f);
            }

            // 饱和度/明度方块
            const D2D1_RECT_F square = SquareRect();
            if (g_squareBitmap)
            {
                target->DrawBitmap(g_squareBitmap, square, 1.0f,
                                   D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
            Draw::StrokeRect(target, square, ColorUtil::FromArgb(0x33FFFFFF), g_scale, 1.0f);

            {
                const float half = Constant::SquareSize / 2.0f;
                const float sx = (Constant::RingCenterX - half) + g_saturation * Constant::SquareSize;
                const float sy = (Constant::RingCenterY - half) + (1.0f - g_value) * Constant::SquareSize;
                Draw::EllipseRing(target, sx, sy, 6.5f, ColorUtil::FromArgb(0xFFFFFFFF), g_scale, 2.2f);
                Draw::EllipseRing(target, sx, sy, 7.8f, ColorUtil::FromArgb(0x66000000), g_scale, 1.0f);
            }

            const D2D1_COLOR_F text = ColorUtil::FromArgb(Theme::TextPrimary());
            const D2D1_COLOR_F accent = ColorUtil::FromArgb(Theme::Accent());
            const D2D1_COLOR_F border = ColorUtil::FromArgb(Theme::InputBorder());
            const D2D1_COLOR_F fill = ColorUtil::FromArgb(Theme::InputFill());

            // HEX 输入
            {
                wchar_t display[24];
                if (g_focus == FocusHex)
                {
                    swprintf_s(display, L"#%s", g_editBuffer.c_str());
                }
                else
                {
                    wchar_t hex[8];
                    swprintf_s(hex, L"%02X%02X%02X", r, g, b);
                    swprintf_s(display, L"#%s", hex);
                }

                Draw::Text(target, L"HEX:", Util::Rect(398, 54, 50, 32, g_scale),
                           text, 16.0f, g_scale);

                const D2D1_RECT_F rect = HexRowRect();
                Draw::FillRect(target, rect, fill, g_scale, 3.0f);
                Draw::StrokeRect(target, rect,
                                 (g_focus == FocusHex) ? accent : border,
                                 g_scale, (g_focus == FocusHex) ? 2.0f : 1.0f, 3.0f);
                Draw::Text(target, display, rect, text, 16.0f, g_scale,
                           DWRITE_TEXT_ALIGNMENT_CENTER);
            }

            // R/G/B 输入
            {
                const wchar_t *labels[3] = { L"R:", L"G:", L"B:" };
                const int values[3] = { r, g, b };

                for (int i = 0; i < 3; ++i)
                {
                    const float centerY = RgbRowCenterY(i);
                    Draw::Text(target, labels[i],
                               D2D1::RectF(398 * g_scale, (centerY - 16) * g_scale,
                                           448 * g_scale, (centerY + 16) * g_scale),
                               text, 16.0f, g_scale);

                    const D2D1_RECT_F rect = RgbRowRect(i);
                    Draw::FillRect(target, rect, fill, g_scale, 3.0f);
                    Draw::StrokeRect(target, rect,
                                     (g_focus == i) ? accent : border,
                                     g_scale, (g_focus == i) ? 2.0f : 1.0f, 3.0f);

                    wchar_t buffer[16];
                    if (g_focus == i)
                    {
                        wcscpy_s(buffer, g_editBuffer.c_str());
                    }
                    else
                    {
                        swprintf_s(buffer, L"%d", values[i]);
                    }

                    Draw::Text(target, buffer, rect, text, 18.0f, g_scale,
                               DWRITE_TEXT_ALIGNMENT_CENTER);
                }
            }

            // 颜色预览
            {
                Draw::Text(target, L"颜色:", Util::Rect(378, 294, 70, 32, g_scale),
                           text, 16.0f, g_scale);

                const D2D1_RECT_F rect = PreviewRect();
                Draw::FillRect(target, rect, ColorUtil::FromArgb(ColorUtil::ToArgb(r, g, b)),
                               g_scale, 3.0f);
                Draw::StrokeRect(target, rect, ColorUtil::FromArgb(Theme::PreviewBorder()),
                                 g_scale, 1.0f, 3.0f);
            }

            // 状态文字
            if (!g_statusText.empty())
            {
                Draw::Text(target, g_statusText.c_str(), Util::Rect(16, 332, 400, 20, g_scale),
                           ColorUtil::FromArgb(g_statusColor ? g_statusColor : Theme::StatusOk()),
                           11.0f, g_scale);
            }
        }

        void DrawScene(ID2D1RenderTarget *target)
        {
            BuildRingBitmap(target);
            BuildSquareBitmap(target);

            int r, g, b;
            CurrentRgb(r, g, b);

            target->Clear(ColorUtil::FromArgb(Theme::Background()));
            DrawChromeButtons(target);
            DrawPicker(target, r, g, b);
        }

        // ---- 窗口 ----

        void CenterOnScreen(HWND hwnd)
        {
            RECT workArea;
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
            const int x = (workArea.left + workArea.right) / 2 - g_windowWidth / 2;
            const int y = (workArea.top + workArea.bottom) / 2 - g_windowHeight / 2;
            SetWindowPos(hwnd, nullptr, x, y, g_windowWidth, g_windowHeight,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }

        void BeginClose(HWND hwnd)
        {
            if (g_closing)
            {
                return;
            }
            g_closing = true;
            // 直接销毁：DWM 会播放系统关闭动画
            DestroyWindow(hwnd);
        }

        LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
        {
            switch (msg)
            {
            case WM_NCCALCSIZE:
                // 去掉系统标题栏与边框：客户区 == 窗口矩形
                if (wParam)
                {
                    auto *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(lParam);
                    RECT windowRect;
                    GetWindowRect(hwnd, &windowRect);
                    params->rgrc[0] = windowRect;
                    return 0;
                }
                else
                {
                    auto *clientRect = reinterpret_cast<RECT *>(lParam);
                    RECT windowRect;
                    GetWindowRect(hwnd, &windowRect);
                    *clientRect = windowRect;
                    return 0;
                }

            case WM_ACTIVATE:
                Theme::ApplyWindowChrome(hwnd);
                break;

            case WM_NCACTIVATE:
                // 拦截非客户区激活重绘：否则失焦时 DWM 会重新画出系统标题栏
                Theme::ApplyWindowChrome(hwnd);
                return TRUE;

            case WM_MOUSEMOVE:
            {
                if (!g_tracking)
                {
                    TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd, 0 };
                    TrackMouseEvent(&track);
                    g_tracking = true;
                }
                g_mouse.x = GET_X_LPARAM(lParam);
                g_mouse.y = GET_Y_LPARAM(lParam);

                if (g_drag != DragNone)
                {
                    ApplyDrag(g_mouse);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            case WM_MOUSELEAVE:
                g_tracking = false;
                g_mouse.x = g_mouse.y = -9999;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;

            case WM_LBUTTONDOWN:
            {
                POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                g_mouse = point;
                g_focus = FocusNone;

                const float mx = point.x / g_scale;
                const float my = point.y / g_scale;
                const float dx = mx - Constant::RingCenterX;
                const float dy = my - Constant::RingCenterY;
                const float radius = sqrtf(dx * dx + dy * dy);
                const float half = Constant::SquareSize / 2.0f;

                if (Util::PointInRect(CloseButtonRect(), point))
                {
                    g_press = PressClose;
                    SetCapture(hwnd);
                }
                else if (Util::PointInRect(PinButtonRect(), point))
                {
                    g_press = PressPin;
                    SetCapture(hwnd);
                }
                else if (Util::PointInRect(GithubButtonRect(), point))
                {
                    g_press = PressGithub;
                    SetCapture(hwnd);
                }
                else if (Util::PointInRect(AboutButtonRect(), point))
                {
                    g_press = PressAbout;
                    SetCapture(hwnd);
                }
                else if (radius >= Constant::RingInner && radius <= Constant::RingOuter)
                {
                    g_drag = DragHue;
                    ApplyDrag(point);
                    SetCapture(hwnd);
                }
                else if (mx >= Constant::RingCenterX - half && mx <= Constant::RingCenterX + half &&
                         my >= Constant::RingCenterY - half && my <= Constant::RingCenterY + half)
                {
                    g_drag = DragSquare;
                    ApplyDrag(point);
                    SetCapture(hwnd);
                }
                else
                {
                    for (int i = 0; i < 3; ++i)
                    {
                        if (Util::PointInRect(RgbRowRect(i), point))
                        {
                            g_focus = i;
                            int r, g, b;
                            CurrentRgb(r, g, b);
                            const int value = (i == 0) ? r : ((i == 1) ? g : b);
                            wchar_t buffer[8];
                            swprintf_s(buffer, L"%d", value);
                            g_editBuffer = buffer;
                        }
                    }

                    if (Util::PointInRect(HexRowRect(), point))
                    {
                        g_focus = FocusHex;
                        int r, g, b;
                        CurrentRgb(r, g, b);
                        wchar_t buffer[8];
                        swprintf_s(buffer, L"%02X%02X%02X", r, g, b);
                        g_editBuffer = buffer;
                    }
                }

                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            case WM_LBUTTONUP:
            {
                POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                const int pressed = g_press;
                g_press = PressNone;

                if (pressed)
                {
                    ReleaseCapture();

                    if (pressed == PressClose && Util::PointInRect(CloseButtonRect(), point))
                    {
                        BeginClose(hwnd);
                        return 0;
                    }
                    if (pressed == PressPin && Util::PointInRect(PinButtonRect(), point))
                    {
                        g_alwaysOnTop = !g_alwaysOnTop;
                        SetWindowPos(hwnd, g_alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                    }
                    if (pressed == PressGithub && Util::PointInRect(GithubButtonRect(), point))
                    {
                        UrlLauncher::Open(Constant::RepositoryUrl);
                    }
                    if (pressed == PressAbout && Util::PointInRect(AboutButtonRect(), point))
                    {
                        AboutWindow::Show(hwnd);
                    }
                }
                else if (g_drag != DragNone)
                {
                    g_drag = DragNone;
                    ReleaseCapture();
                    ApplyColor();
                }

                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            case WM_CHAR:
            {
                if (g_focus == FocusNone)
                {
                    return 0;
                }

                if (wParam == 8) // 退格
                {
                    if (!g_editBuffer.empty())
                    {
                        g_editBuffer.pop_back();
                    }
                }
                else if (wParam == 13) // 回车
                {
                    if (g_focus == FocusHex)
                    {
                        std::wstring hex;
                        for (wchar_t c : g_editBuffer)
                        {
                            if (iswxdigit(c))
                            {
                                hex.push_back(c);
                            }
                        }
                        if (hex.size() > 6)
                        {
                            hex = hex.substr(hex.size() - 6);
                        }
                        if (hex.size() == 6)
                        {
                            const int r = static_cast<int>(wcstol(hex.substr(0, 2).c_str(), nullptr, 16));
                            const int g = static_cast<int>(wcstol(hex.substr(2, 2).c_str(), nullptr, 16));
                            const int b = static_cast<int>(wcstol(hex.substr(4, 2).c_str(), nullptr, 16));
                            ColorUtil::RgbToHsv(r, g, b, g_hue, g_saturation, g_value);
                            ApplyColor();
                        }
                    }
                    else
                    {
                        int value = _wtoi(g_editBuffer.c_str());
                        value = std::clamp(value, 0, 255);

                        int r, g, b;
                        CurrentRgb(r, g, b);
                        if (g_focus == FocusRed)        r = value;
                        else if (g_focus == FocusGreen) g = value;
                        else                            b = value;

                        ColorUtil::RgbToHsv(r, g, b, g_hue, g_saturation, g_value);
                        ApplyColor();
                    }
                    g_focus = FocusNone;
                }
                else if (wParam == 27) // Esc
                {
                    g_focus = FocusNone;
                }
                else if (g_focus == FocusHex)
                {
                    if (iswxdigit(static_cast<wchar_t>(wParam)))
                    {
                        if (g_editBuffer.size() >= 6)
                        {
                            g_editBuffer.erase(0, 1);
                        }
                        g_editBuffer.push_back(
                            static_cast<wchar_t>(towupper(static_cast<wchar_t>(wParam))));
                    }
                }
                else
                {
                    if (wParam >= L'0' && wParam <= L'9' && g_editBuffer.size() < 3)
                    {
                        g_editBuffer.push_back(static_cast<wchar_t>(wParam));
                    }
                }

                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            case WM_KEYDOWN:
                if (wParam == VK_ESCAPE)
                {
                    if (g_focus != FocusNone)
                    {
                        g_focus = FocusNone;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    else
                    {
                        BeginClose(hwnd);
                    }
                    return 0;
                }
                break;

            case WM_NCHITTEST:
            {
                if (g_closing)
                {
                    return HTCLIENT;
                }

                POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ScreenToClient(hwnd, &point);

                const float mx = point.x / g_scale;
                const float my = point.y / g_scale;
                const float dx = mx - Constant::RingCenterX;
                const float dy = my - Constant::RingCenterY;
                const float radius = sqrtf(dx * dx + dy * dy);
                const float half = Constant::SquareSize / 2.0f;

                bool interactive = (radius >= Constant::RingInner && radius <= Constant::RingOuter) ||
                                   (mx >= Constant::RingCenterX - half && mx <= Constant::RingCenterX + half &&
                                    my >= Constant::RingCenterY - half && my <= Constant::RingCenterY + half) ||
                                   Util::PointInRect(CloseButtonRect(), point) ||
                                   Util::PointInRect(PinButtonRect(), point) ||
                                   Util::PointInRect(GithubButtonRect(), point) ||
                                   Util::PointInRect(AboutButtonRect(), point) ||
                                   Util::PointInRect(PreviewRect(), point) ||
                                   Util::PointInRect(HexRowRect(), point);

                for (int i = 0; i < 3 && !interactive; ++i)
                {
                    if (Util::PointInRect(RgbRowRect(i), point))
                    {
                        interactive = true;
                    }
                }

                if (my < 36.0f && !interactive)
                {
                    return HTCAPTION;
                }
                return HTCLIENT;
            }

            case WM_SETCURSOR:
                if (LOWORD(lParam) == HTCLIENT)
                {
                    POINT point;
                    GetCursorPos(&point);
                    ScreenToClient(hwnd, &point);

                    const bool textInput = Util::PointInRect(HexRowRect(), point) ||
                                           Util::PointInRect(RgbRowRect(0), point) ||
                                           Util::PointInRect(RgbRowRect(1), point) ||
                                           Util::PointInRect(RgbRowRect(2), point);

                    const float mx = point.x / g_scale;
                    const float my = point.y / g_scale;
                    const float dx = mx - Constant::RingCenterX;
                    const float dy = my - Constant::RingCenterY;
                    const float radius = sqrtf(dx * dx + dy * dy);
                    const float half = Constant::SquareSize / 2.0f;

                    const bool clickable =
                        (radius >= Constant::RingInner && radius <= Constant::RingOuter) ||
                        (mx >= Constant::RingCenterX - half && mx <= Constant::RingCenterX + half &&
                         my >= Constant::RingCenterY - half && my <= Constant::RingCenterY + half) ||
                        Util::PointInRect(CloseButtonRect(), point) ||
                        Util::PointInRect(PinButtonRect(), point) ||
                        Util::PointInRect(GithubButtonRect(), point) ||
                        Util::PointInRect(AboutButtonRect(), point);

                    SetCursor(LoadCursorW(nullptr,
                                          textInput ? IDC_IBEAM : (clickable ? IDC_HAND : IDC_ARROW)));
                    return TRUE;
                }
                break;

            case WM_ERASEBKGND:
                return 1;

            case WM_SIZE:
                if (g_target)
                {
                    g_target->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
                }
                return 0;

            case WM_THEMECHANGED:
            case WM_SETTINGCHANGE:
                Theme::ReadFromSystem();
                Theme::ApplyWindowChrome(hwnd);
                AboutWindow::OnSystemThemeChanged();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;

            case WM_CLOSE:
                BeginClose(hwnd);
                return 0;

            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                BeginPaint(hwnd, &ps);

                if (!g_target && g_factory)
                {
                    RECT rc;
                    GetClientRect(hwnd, &rc);
                    g_factory->CreateHwndRenderTarget(
                        D2D1::RenderTargetProperties(),
                        D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right, rc.bottom)),
                        &g_target);
                }

                if (g_target)
                {
                    // 位图必须在 BeginDraw 之前构建
                    BuildRingBitmap(g_target);
                    BuildSquareBitmap(g_target);

                    g_target->BeginDraw();
                    DrawScene(g_target);
                    if (g_target->EndDraw() == D2DERR_RECREATE_TARGET)
                    {
                        Util::SafeRelease(g_target);
                    }
                }

                EndPaint(hwnd, &ps);
                return 0;
            }

            case WM_DESTROY:
                AboutWindow::Close();
                PostQuitMessage(0);
                return 0;
            }

            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    int Run(HINSTANCE instance)
    {
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_factory);
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&g_wic));

        RestoreSavedState();

        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = instance;
        wc.lpszClassName = Constant::MainClassName;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        RegisterClassExW(&wc);

        const UINT systemDpi = GetDpiForSystem();
        g_scale = (systemDpi > 0) ? (systemDpi / 96.0f) : 1.0f;

        // 标准窗口底层：DWM 阴影 + 圆角 + 系统开关动画；标题栏由 WM_NCCALCSIZE 去掉
        const DWORD style = WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_MINIMIZEBOX);

        // WM_NCCALCSIZE 让客户区等于窗口矩形，故窗口尺寸即画布尺寸
        g_windowWidth = static_cast<int>(Constant::CanvasWidth * g_scale);
        g_windowHeight = static_cast<int>(Constant::CanvasHeight * g_scale);

        g_window = CreateWindowExW(WS_EX_APPWINDOW, Constant::MainClassName, L"GpuColorLite",
                                   style, 0, 0, g_windowWidth, g_windowHeight,
                                   nullptr, nullptr, instance, nullptr);
        if (!g_window)
        {
            return 1;
        }

        CenterOnScreen(g_window);
        Theme::ApplyWindowChrome(g_window);
        SetWindowPos(g_window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

        // 预建资源与预热一帧，避免动画过程中出现空白
        Draw::Prepare(g_factory);
        {
            RECT rc;
            GetClientRect(g_window, &rc);
            if (!g_target)
            {
                g_factory->CreateHwndRenderTarget(
                    D2D1::RenderTargetProperties(),
                    D2D1::HwndRenderTargetProperties(g_window, D2D1::SizeU(rc.right, rc.bottom)),
                    &g_target);
            }
            if (g_target)
            {
                BuildRingBitmap(g_target);
                BuildSquareBitmap(g_target);
                g_target->BeginDraw();
                DrawScene(g_target);
                g_target->EndDraw();
            }
        }

        // 打开动画由 DWM 提供（窗口带 WS_OVERLAPPEDWINDOW 时系统自动播放）
        ShowWindow(g_window, SW_SHOW);
        UpdateWindow(g_window);

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        Util::SafeRelease(g_target);
        Util::SafeRelease(g_ringBitmap);
        Util::SafeRelease(g_ringWic);
        Util::SafeRelease(g_squareBitmap);
        Util::SafeRelease(g_squareWic);
        Util::SafeRelease(g_wic);
        Util::SafeRelease(g_factory);

        return static_cast<int>(msg.wParam);
    }

    bool RenderToPng(const wchar_t *path, float scale)
    {
        if (!g_factory)
        {
            D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_factory);
        }
        if (!g_wic)
        {
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                             IID_PPV_ARGS(&g_wic));
        }
        if (!g_factory || !g_wic || !path)
        {
            return false;
        }

        g_scale = scale;

        const UINT width = static_cast<UINT>(Constant::CanvasWidth * scale);
        const UINT height = static_cast<UINT>(Constant::CanvasHeight * scale);

        IWICBitmap *bitmap = nullptr;
        g_wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                            WICBitmapCacheOnLoad, &bitmap);

        ID2D1RenderTarget *target = nullptr;
        g_factory->CreateWicBitmapRenderTarget(
            bitmap,
            D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f),
            &target);

        if (!target)
        {
            Util::SafeRelease(bitmap);
            return false;
        }

        Draw::Prepare(g_factory);
        BuildRingBitmap(target);
        BuildSquareBitmap(target);

        target->BeginDraw();
        DrawScene(target);
        target->EndDraw();

        IWICStream *stream = nullptr;
        g_wic->CreateStream(&stream);
        stream->InitializeFromFilename(path, GENERIC_WRITE);

        IWICBitmapEncoder *encoder = nullptr;
        g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        encoder->Initialize(stream, WICBitmapEncoderNoCache);

        IWICBitmapFrameEncode *frame = nullptr;
        IPropertyBag2 *properties = nullptr;
        encoder->CreateNewFrame(&frame, &properties);
        frame->Initialize(properties);
        frame->SetSize(width, height);

        WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
        frame->SetPixelFormat(&format);
        frame->WriteSource(bitmap, nullptr);
        frame->Commit();
        encoder->Commit();

        Util::SafeRelease(frame);
        Util::SafeRelease(properties);
        Util::SafeRelease(encoder);
        Util::SafeRelease(stream);
        Util::SafeRelease(target);
        Util::SafeRelease(bitmap);

        return true;
    }
}

// ============================================================================
//  9. wWinMain —— 入口与命令行
// ============================================================================

#ifndef GPUCOLORLITE_BUILD_DLL

namespace
{
    bool ParseByte(const wchar_t *text, unsigned char &value)
    {
        if (!text || !*text)
        {
            return false;
        }

        wchar_t *end = nullptr;
        const unsigned long parsed = std::wcstoul(text, &end, 10);
        if (!end || *end != L'\0' || parsed > 255)
        {
            return false;
        }

        value = static_cast<unsigned char>(parsed);
        return true;
    }

    bool ParseHexColor(const wchar_t *text, unsigned char &r, unsigned char &g, unsigned char &b)
    {
        if (!text)
        {
            return false;
        }

        std::wstring_view hex(text);
        if (!hex.empty() && hex.front() == L'#')
        {
            hex.remove_prefix(1);
        }
        if (hex.size() != 6)
        {
            return false;
        }

        const std::wstring owned(hex);
        wchar_t *end = nullptr;
        const unsigned long parsed = std::wcstoul(owned.c_str(), &end, 16);
        if (!end || *end != L'\0')
        {
            return false;
        }

        r = static_cast<unsigned char>((parsed >> 16) & 0xFF);
        g = static_cast<unsigned char>((parsed >> 8) & 0xFF);
        b = static_cast<unsigned char>(parsed & 0xFF);
        return true;
    }

    int ApplyAndPersist(unsigned char r, unsigned char g, unsigned char b)
    {
        const GpuColor::Result result = GpuColor::SetColor(r, g, b);

        float hue = 0.0f, saturation = 0.0f, value = 0.0f;
        ColorUtil::RgbToHsv(r, g, b, hue, saturation, value);
        GpuColor::SaveHsv(static_cast<unsigned>(hue),
                          static_cast<unsigned>(saturation * 1000.0f),
                          static_cast<unsigned>(value * 1000.0f));

        if (!result.Success)
        {
            GpuColor::Log(result.Error);
            return 1;
        }
        return 0;
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
    {
        return 2;
    }

    bool forceLight = false;
    bool forceDark = false;
    bool offRequested = false;
    bool hasColor = false;
    unsigned char r = 0, g = 0, b = 0;
    const wchar_t *shotPath = nullptr;

    for (int i = 1; i < argc; ++i)
    {
        const std::wstring_view arg(argv[i]);

        if (arg == L"--light")
        {
            forceLight = true;
        }
        else if (arg == L"--dark")
        {
            forceDark = true;
        }
        else if (arg == L"--off")
        {
            offRequested = true;
        }
        else if (arg == L"--shot" && i + 1 < argc)
        {
            shotPath = argv[++i];
        }
        else if (arg == L"--hex" && i + 1 < argc)
        {
            hasColor = ParseHexColor(argv[++i], r, g, b);
        }
        else if (arg == L"--rgb" && i + 3 < argc)
        {
            hasColor = ParseByte(argv[i + 1], r) &&
                       ParseByte(argv[i + 2], g) &&
                       ParseByte(argv[i + 3], b);
            i += 3;
        }
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    Theme::ReadFromSystem();
    if (forceLight)
    {
        Theme::SetLight(true);
    }
    if (forceDark)
    {
        Theme::SetLight(false);
    }

    int exitCode = 0;

    if (offRequested)
    {
        exitCode = ApplyAndPersist(0, 0, 0);
    }
    else if (hasColor)
    {
        exitCode = ApplyAndPersist(r, g, b);
    }
    else if (shotPath)
    {
        exitCode = MainWindow::RenderToPng(shotPath, 1.5f) ? 0 : 1;
    }
    else if (argc > 1)
    {
        GpuColor::Log(L"参数无效。用法: GpuColorLite.exe [R G B | --hex RRGGBB | --off | --shot 文件]");
        exitCode = 2;
    }
    else
    {
        exitCode = MainWindow::Run(instance);
    }

    CoUninitialize();
    LocalFree(argv);
    return exitCode;
}

#else // GPUCOLORLITE_BUILD_DLL —— 导出为 DLL 供其他程序集成

// ============================================================================
//  DLL 导出接口（C ABI，便于任意语言/编译器调用）
//
//  返回值统一为 int：0 = 成功，非 0 = 失败
// ============================================================================

namespace
{
    std::string g_dllLastError;

    void RecordError(const std::wstring &message)
    {
        if (message.empty())
        {
            g_dllLastError.clear();
            return;
        }

        const int length = WideCharToMultiByte(CP_UTF8, 0, message.c_str(), -1,
                                               nullptr, 0, nullptr, nullptr);
        if (length > 1)
        {
            g_dllLastError.resize(static_cast<size_t>(length - 1));
            WideCharToMultiByte(CP_UTF8, 0, message.c_str(), -1,
                                &g_dllLastError[0], length, nullptr, nullptr);
        }
        else
        {
            g_dllLastError.clear();
        }
    }
}

extern "C"
{
    // 设置 GPU 灯效颜色
    __declspec(dllexport) int __stdcall GclSetColor(unsigned char r, unsigned char g,
                                                    unsigned char b)
    {
        const GpuColor::Result result = GpuColor::SetColor(r, g, b);
        if (!result.Success)
        {
            RecordError(result.Error);
        }
        return result.Success ? 0 : 1;
    }

    // 保存 HSV（Hue 0..359，Sat/Val 0..1000）
    __declspec(dllexport) int __stdcall GclSaveHsv(unsigned hue, unsigned saturationPermille,
                                                   unsigned valuePermille)
    {
        return GpuColor::SaveHsv(hue, saturationPermille, valuePermille) ? 0 : 1;
    }

    // 读取 HSV，成功返回 0
    __declspec(dllexport) int __stdcall GclLoadHsv(unsigned *hue, unsigned *saturationPermille,
                                                   unsigned *valuePermille)
    {
        if (!hue || !saturationPermille || !valuePermille)
        {
            return 2;
        }
        return GpuColor::LoadHsv(*hue, *saturationPermille, *valuePermille) ? 0 : 1;
    }

    // 保存 / 读取 RGB
    __declspec(dllexport) int __stdcall GclSaveRgb(unsigned char r, unsigned char g,
                                                   unsigned char b)
    {
        GpuColor::SaveColor(r, g, b);
        return 0;
    }

    __declspec(dllexport) int __stdcall GclLoadRgb(unsigned char *r, unsigned char *g,
                                                   unsigned char *b)
    {
        if (!r || !g || !b)
        {
            return 2;
        }
        return GpuColor::LoadColor(*r, *g, *b) ? 0 : 1;
    }

    // 最近一次失败原因（UTF-8，无需释放，下次调用覆盖）
    __declspec(dllexport) const char *__stdcall GclLastError()
    {
        return g_dllLastError.c_str();
    }

    // 版本号字符串
    __declspec(dllexport) const char *__stdcall GclVersion()
    {
        return "1.0.0";
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        Theme::ReadFromSystem();
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        CoUninitialize();
    }
    return TRUE;
}

#endif // GPUCOLORLITE_BUILD_DLL
