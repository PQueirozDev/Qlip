#pragma once

#include <string>
#include <vector>

#include <windows.h>

// Lista suspensa no tema escuro, aberta abaixo (ou acima) de um seletor.
// Modal como um menu: retorna o índice escolhido, ou -1 se fechada sem escolha (Esc, clique fora).
// Teclado: setas, Home/End, Enter e Esc.
int showDropdown(HWND owner, const RECT& anchorScreen, const std::vector<std::wstring>& items, int current, HFONT font);
