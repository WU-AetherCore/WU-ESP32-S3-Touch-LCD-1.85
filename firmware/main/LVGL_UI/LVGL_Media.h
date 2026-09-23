#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
void Media_Init(void (*home)(void));
void Media_Show(bool video);
void Media_Hide(void);
void Media_Back(void);
void Media_Next(int delta);
void Media_Toggle(void);
void Media_Rescan(void);
void Media_Prepare(void);
void Media_Debug_Files(void);
void Media_Debug_Status(void);
void Media_Debug_Open(unsigned index);
void Media_Debug_List_Page(int delta);
bool Media_Debug_Upload(const char *command);
size_t Media_Debug_Binary(const uint8_t *data,size_t size);
