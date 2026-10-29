#pragma once
#include <windows.h>
#include "navigation_model.h"

void StartSessionNavigation(HWND recipient, UINT completedMessage);
void StopSessionNavigation();
bool NavigateToSession(const Session &session, const std::vector<Session> &peers);
bool SessionNavigationBusy();
std::optional<NavigationResult> TakeNavigationResult();
NavigationResult LastNavigationResult();
int NavigationDiagnostic(unsigned field);
