#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <windows.h>
#include <commctrl.h>

// Biblioteca: lista as gravações e os clipes salvos, com opções para abrir, mostrar na pasta e excluir.
class LibraryWindow {
public:
    struct Folders {
        std::filesystem::path recordings;
        std::filesystem::path clips;
    };

    LibraryWindow(HINSTANCE instance, HWND owner, std::function<Folders()> folders);
    ~LibraryWindow();

    // Mostra a janela e atualiza a lista.
    void open();

private:
    struct Item {
        std::filesystem::path path;
        bool clip = false;
        double durationSeconds = 0.0;
        uint64_t bytes = 0;
        int64_t modifiedFileTime = 0;
    };

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    bool create();
    void layout();
    void refresh();
    void fillList();
    int selectedIndex() const;
    void openSelected();
    void showSelectedInFolder();
    void deleteSelected();
    void drawButton(const DRAWITEMSTRUCT& item) const;
    static LRESULT CALLBACK listSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data);
    LRESULT drawHeader(NMCUSTOMDRAW& draw) const;

    HINSTANCE instance_;
    HWND owner_;
    std::function<Folders()> folders_;
    HWND hwnd_ = nullptr;
    HWND list_ = nullptr;
    HWND openButton_ = nullptr;
    HWND folderButton_ = nullptr;
    HWND deleteButton_ = nullptr;
    HWND refreshButton_ = nullptr;
    HWND countLabel_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH surfaceBrush_ = nullptr;
    HFONT font_ = nullptr;
    HFONT buttonFont_ = nullptr;
    HFONT headerFont_ = nullptr;
    std::vector<Item> items_;
};
