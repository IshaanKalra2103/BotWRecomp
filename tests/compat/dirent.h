#pragma once
#include <string>
#include <vector>
struct dirent { char d_name[256]; };
struct DIR { std::vector<std::string> names; size_t pos = 0; dirent entry{}; };
DIR* opendir(const char*);
dirent* readdir(DIR*);
int closedir(DIR*);
