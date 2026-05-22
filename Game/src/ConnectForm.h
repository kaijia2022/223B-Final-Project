#pragma once
#include "raylib.h"
#include <string>


enum class NetworkRole { NONE, HOST, CLIENT };

void RunConnectionScreen(std::string& outIpAddress, NetworkRole& outRole);