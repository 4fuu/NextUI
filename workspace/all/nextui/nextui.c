#include <stdio.h>
#include <stdlib.h>
#include <msettings.h>
#include <sys/types.h>
#include <dirent.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <libgen.h>  // For dirname()
#include "defines.h"
#include "api.h"
#include "utils.h"
#include "config.h"
#include "app_capabilities.h"
#include "i18n.h"
#include "ui_overlay.h"
#include <sys/resource.h>
#include <pthread.h>
#include <assert.h>
#include <limits.h>
#include "pak_manifest.h"
#include "app_state.h"

///////////////////////////////////////

typedef struct Array {
	int count;
	int capacity;
	void** items;
} Array;

static Array* Array_new(void) {
	Array* self = malloc(sizeof(Array));
	self->count = 0;
	self->capacity = 8;
	self->items = malloc(sizeof(void*) * self->capacity);
	return self;
}
static void Array_push(Array* self, void* item) {
	if (self->count>=self->capacity) {
		self->capacity *= 2;
		self->items = realloc(self->items, sizeof(void*) * self->capacity);
	}
	self->items[self->count++] = item;
}
static void Array_unshift(Array* self, void* item) {
	if (self->count==0) return Array_push(self, item);
	Array_push(self, NULL); // ensures we have enough capacity
	for (int i=self->count-2; i>=0; i--) {
		self->items[i+1] = self->items[i];
	}
	self->items[0] = item;
}
static void* Array_pop(Array* self) {
	if (self->count==0) return NULL;
	return self->items[--self->count];
}
static void Array_remove(Array* self, void* item) {
	if (self->count==0 || item == NULL)
		return;
	int i = 0;
	while (self->items[i] != item) i++;
	for (int j = i; j < self->count-1; j++)
		self->items[j] = self->items[j+1];
	self->count--;
}
static void Array_reverse(Array* self) {
	int end = self->count-1;
	int mid = self->count/2;
	for (int i=0; i<mid; i++) {
		void* item = self->items[i];
		self->items[i] = self->items[end-i];
		self->items[end-i] = item;
	}
}
static void Array_free(Array* self) {
	free(self->items);
	free(self);
}
static void Array_yoink(Array* self, Array* other) {
	// append entries to self and take ownership
	for (int i = 0; i < other->count; i++)
        Array_push(self, other->items[i]);
    Array_free(other); // `self` now owns the entries
}

static int StringArray_indexOf(Array* self, char* str) {
	for (int i=0; i<self->count; i++) {
		if (exactMatch(self->items[i], str)) return i;
	}
	return -1;
}
static void StringArray_free(Array* self) {
	for (int i=0; i<self->count; i++) {
		free(self->items[i]);
	}
	Array_free(self);
}

///////////////////////////////////////

typedef struct Hash {
	Array* keys;
	Array* values;
} Hash; // not really a hash

static Hash* Hash_new(void) {
	Hash* self = malloc(sizeof(Hash));
	self->keys = Array_new();
	self->values = Array_new();
	return self;
}
static void Hash_free(Hash* self) {
	StringArray_free(self->keys);
	StringArray_free(self->values);
	free(self);
}
static void Hash_set(Hash* self, char* key, char* value) {
	Array_push(self->keys, strdup(key));
	Array_push(self->values, strdup(value));
}
static char* Hash_get(Hash* self, char* key) {
	int i = StringArray_indexOf(self->keys, key);
	if (i==-1) return NULL;
	return self->values->items[i];
}

///////////////////////////////////////

enum EntryType {
	ENTRY_DIR,
	ENTRY_PAK,
	ENTRY_DIP,
};
typedef struct Entry {
	char* path;
	char* name;
	char* unique;
	char* icon; // optional pak.json icon path; NULL for legacy paks and non-pak entries
	unsigned capabilities; // PAK_CAPABILITY_* declared by pak.json; 0 for legacy paks
	int type;
	int alpha; // index in parent Directory's alphas Array, which points to the index of an Entry in its entries Array :sweat_smile:
} Entry;

static Entry* Entry_new(char* path, int type) {
	char display_name[256];
	getDisplayName(path, display_name);
	Entry* self = malloc(sizeof(Entry));
	self->path = strdup(path);
	self->name = strdup(display_name);
	self->unique = NULL;
	self->icon = NULL;
	self->capabilities = 0;
	self->type = type;
	self->alpha = 0;

	// pak.json is optional metadata: a legacy launch.sh-only pak keeps the
	// directory name, and an absent or malformed manifest never changes how
	// the pak is listed or launched.
	if (type==ENTRY_PAK) {
		PakManifest manifest;
		if (pak_manifest_load(path, &manifest)==PAK_MANIFEST_OK) {
			self->capabilities = manifest.capabilities;
			if (manifest.name[0]) {
				free(self->name);
				self->name = strdup(manifest.name);
			}
			if (manifest.icon[0]) {
				if (manifest.icon[0]=='/') {
					self->icon = strdup(manifest.icon);
				}
				else {
					char icon_path[MAX_PATH];
					int len = snprintf(icon_path, sizeof(icon_path), "%s/%s", path, manifest.icon);
					if (len>0 && (size_t)len<sizeof(icon_path)) self->icon = strdup(icon_path);
				}
			}
		}
	}
	return self;
}

static Entry* Entry_newNamed(char* path, int type, const char* displayName) {
	Entry *self = Entry_new(path, type);
	free(self->name);
	self->name = strdup(displayName);
	return self;
}

static void Entry_free(Entry* self) {
	free(self->path);
	free(self->name);
	if (self->unique) free(self->unique);
	if (self->icon) free(self->icon);
	free(self);
}

static int EntryArray_indexOf(Array* self, char* path) {
	for (int i=0; i<self->count; i++) {
		Entry* entry = self->items[i];
		if (exactMatch(entry->path, path)) return i;
	}
	return -1;
}
static int EntryArray_sortEntry(const void* a, const void* b) {
	Entry* item1 = *(Entry**)a;
	Entry* item2 = *(Entry**)b;
	return strcasecmp(item1->name, item2->name);
}
static void EntryArray_sort(Array* self) {
	qsort(self->items, self->count, sizeof(void*), EntryArray_sortEntry);
}

static void EntryArray_free(Array* self) {
	for (int i=0; i<self->count; i++) {
		Entry_free(self->items[i]);
	}
	Array_free(self);
}

///////////////////////////////////////

#define INT_ARRAY_MAX 27
typedef struct IntArray {
	int count;
	int items[INT_ARRAY_MAX];
} IntArray;
static IntArray* IntArray_new(void) {
	IntArray* self = malloc(sizeof(IntArray));
	self->count = 0;
	memset(self->items, 0, sizeof(int) * INT_ARRAY_MAX);
	return self;
}
static void IntArray_push(IntArray* self, int i) {
	self->items[self->count++] = i;
}
static void IntArray_free(IntArray* self) {
	free(self);
}

///////////////////////////////////////

typedef struct Directory {
	char* path;
	char* name;
	Array* entries;
	IntArray* alphas;
	// rendering
	int selected;
	int start;
	int end;
} Directory;

static int getIndexChar(char* str) {
	char i = 0;
	char c = tolower(str[0]);
	if (c>='a' && c<='z') i = (c-'a')+1;
	return i;
}

static void getUniqueName(Entry* entry, char* out_name) {
	char* filename = strrchr(entry->path, '/')+1;

	char *tmp;
	strcpy(out_name, entry->name);
	tmp = out_name + strlen(out_name);
	strcpy(tmp, " (");
	tmp = out_name + strlen(out_name);
	strcpy(tmp, filename);
	tmp = out_name + strlen(out_name);
	strcpy(tmp, ")");
}

static void Directory_index(Directory* self) {
    int is_collection = prefixMatch(COLLECTIONS_PATH, self->path);
    int skip_index = exactMatch(FAUX_RECENT_PATH, self->path) || is_collection; // not alphabetized

    Hash* map = NULL;
    char map_path[256];
    sprintf(map_path, "%s/map.txt", is_collection ? COLLECTIONS_PATH : self->path);

    if (exists(map_path)) {
        FILE* file = fopen(map_path, "r");
        if (file) {
            map = Hash_new();
            char line[256];
            while (fgets(line, 256, file) != NULL) {
                normalizeNewline(line);
                trimTrailingNewlines(line);
                if (strlen(line) == 0) continue; // skip empty lines

                char* tmp = strchr(line, '\t');
                if (tmp) {
                    tmp[0] = '\0';
                    char* key = line;
                    char* value = tmp + 1;
                    Hash_set(map, key, strdup(value)); // Ensure strdup to store value properly
                }
            }
            fclose(file);

            int resort = 0;
            int filter = 0;
            for (int i = 0; i < self->entries->count; i++) {
                Entry* entry = self->entries->items[i];
                char* filename = strrchr(entry->path, '/') + 1;
                char* alias = Hash_get(map, filename);
                if (alias) {
                    free(entry->name);  // Free before overwriting
                    entry->name = strdup(alias);
                    resort = 1;
                    if (!filter && hide(entry->name)) filter = 1;
                }
            }

            if (filter) {
                Array* entries = Array_new();
                for (int i = 0; i < self->entries->count; i++) {
                    Entry* entry = self->entries->items[i];
                    if (hide(entry->name)) {
                        Entry_free(entry); // Ensure Entry_free handles all memory cleanup
                    } else {
                        Array_push(entries, entry);
                    }
                }
                Array_free(self->entries);
                self->entries = entries;
            }
            if (resort) EntryArray_sort(self->entries);
        }
    }

    Entry* prior = NULL;
    int alpha = -1;
    int index = 0;
    for (int i = 0; i < self->entries->count; i++) {
        Entry* entry = self->entries->items[i];
        if (map) {
            char* filename = strrchr(entry->path, '/') + 1;
            char* alias = Hash_get(map, filename);
            if (alias) {
                free(entry->name);  // Free before overwriting
                entry->name = strdup(alias);
            }
        }

        if (prior != NULL && exactMatch(prior->name, entry->name)) {
            free(prior->unique);
            free(entry->unique);
            prior->unique = NULL;
            entry->unique = NULL;

            char* prior_filename = strrchr(prior->path, '/') + 1;
            char* entry_filename = strrchr(entry->path, '/') + 1;
            if (exactMatch(prior_filename, entry_filename)) {
                char prior_unique[256];
                char entry_unique[256];
                getUniqueName(prior, prior_unique);
                getUniqueName(entry, entry_unique);

                prior->unique = strdup(prior_unique);
                entry->unique = strdup(entry_unique);
            } else {
                prior->unique = strdup(prior_filename);
                entry->unique = strdup(entry_filename);
            }
        }

        if (!skip_index) {
            int a = getIndexChar(entry->name);
            if (a != alpha) {
                index = self->alphas->count;
                IntArray_push(self->alphas, i);
                alpha = a;
            }
            entry->alpha = index;
        }

        prior = entry;
    }

    if (map) Hash_free(map);  // Free the map at the end
}

static Array* getRoot(void);
static Array* getRecents(void);
static Array* getCollection(char* path);
static Array* getEntries(char* path);

static Directory* Directory_new(char* path, int selected) {
	char display_name[256];
	getDisplayName(path, display_name);

	Directory* self = malloc(sizeof(Directory));
	self->path = strdup(path);
	self->name = strdup(display_name);
	if (exactMatch(path, SDCARD_PATH)) {
		self->entries = getRoot();
	}
	else if (exactMatch(path, FAUX_RECENT_PATH)) {
		self->entries = getRecents();
	}
	else if (!exactMatch(path, COLLECTIONS_PATH) && prefixMatch(COLLECTIONS_PATH, path) && suffixMatch(".txt", path)) {
		self->entries = getCollection(path);
	}
	else {
		self->entries = getEntries(path);
	}
	self->alphas = IntArray_new();
	self->selected = selected;
	Directory_index(self);
	return self;
}
static void Directory_free(Directory* self) {
	free(self->path);
	free(self->name);
	EntryArray_free(self->entries);
	IntArray_free(self->alphas);
	free(self);
}

static void DirectoryArray_pop(Array* self) {
	Directory_free(Array_pop(self));
}
static void DirectoryArray_free(Array* self) {
	for (int i=0; i<self->count; i++) {
		Directory_free(self->items[i]);
	}
	Array_free(self);
}

///////////////////////////////////////

typedef struct Recent {
	char* path; // NOTE: this is without the SDCARD_PATH prefix!
	char* alias;
	int available;
} Recent;
 // yiiikes
static char* recent_alias = NULL;

static Recent* Recent_new(char* path, char* alias) {
	Recent* self = malloc(sizeof(Recent));

	char sd_path[256];
	sprintf(sd_path, "%s%s", SDCARD_PATH, path);

	self->path = strdup(path);
	self->alias = alias ? strdup(alias) : NULL;

	// Only paks are launchable now; anything else (eg. a leftover rom path)
	// stays in the list but is shown as unavailable.
	if (suffixMatch(".pak", sd_path)) {
		char launch_path[256];
		snprintf(launch_path, sizeof(launch_path), "%s/launch.sh", sd_path);
		self->available = exists(launch_path);
	}
	else {
		self->available = 0;
	}
	return self;
}
static void Recent_free(Recent* self) {
	free(self->path);
	if (self->alias) free(self->alias);
	free(self);
}

static int RecentArray_indexOf(Array* self, char* str) {
	for (int i=0; i<self->count; i++) {
		Recent* item = self->items[i];
		if (exactMatch(item->path, str)) return i;
	}
	return -1;
}
static void RecentArray_free(Array* self) {
	for (int i=0; i<self->count; i++) {
		Recent_free(self->items[i]);
	}
	Array_free(self);
}

///////////////////////////////////////

static Directory* top;
static Array* stack; // DirectoryArray
static Array* recents; // RecentArray
static Array *quick; // EntryArray
static Array *quickActions; // EntryArray

static int quit = 0;
static int pak_can_resume = 0; // B4: selected pak has declared saves + a valid B3 state
static int should_resume_pak = 0; // set to 1 on BTN_RESUME but only if pak_can_resume==1
static char pak_resume_cache_path[256]; // last inspected pak; avoids duplicate hashes on redraw/open
static int pak_resume_cache_result = 0;
static int pak_resume_cache_valid = 0;
static int simple_mode = 0;
static int switcher_selected = 0;
static int animationdirection = 0;

static int restore_depth = -1;
static int restore_relative = -1;
static int restore_selected = 0;
static int restore_start = 0;
static int restore_end = 0;
static int startgame = 0;
///////////////////////////////////////

#define MAX_RECENTS 24 // a multiple of all menu rows
static void saveRecents(void) {
	FILE* file = fopen(RECENT_PATH, "w");
	if (file) {
		for (int i=0; i<recents->count; i++) {
			Recent* recent = recents->items[i];
			fputs(recent->path, file);
			if (recent->alias) {
				fputs("\t", file);
				fputs(recent->alias, file);
			}
			putc('\n', file);
		}
		fclose(file);
	}
}
static void addRecent(char* path, char* alias) {
	path += strlen(SDCARD_PATH); // makes paths platform agnostic
	int id = RecentArray_indexOf(recents, path);
	if (id==-1) { // add
		while (recents->count>=MAX_RECENTS) {
			Recent_free(Array_pop(recents));
		}
		Array_unshift(recents, Recent_new(path, alias));
	}
	else if (id>0) { // bump to top
		for (int i=id; i>0; i--) {
			void* tmp = recents->items[i-1];
			recents->items[i-1] = recents->items[i];
			recents->items[i] = tmp;
		}
	}
	saveRecents();
}

static Entry* entryFromPakName(char* pak_name)
{
	char pak_path[256];
	// Check in Tools
	sprintf(pak_path, "%s/Tools/%s/%s.pak", SDCARD_PATH, PLATFORM, pak_name);
	if(exists(pak_path))
		return Entry_newNamed(pak_path, ENTRY_PAK, pak_name);

	return NULL;
}

static int hasRecents(void) {
	LOG_info("hasRecents %s\n", RECENT_PATH);
	int has = 0;
	RecentArray_free(recents);
	recents = Array_new();

	FILE *file = fopen(RECENT_PATH, "r"); // newest at top
	if (file) {
		char line[256];
		while (fgets(line,256,file)!=NULL) {
			normalizeNewline(line);
			trimTrailingNewlines(line);
			if (strlen(line)==0) continue; // skip empty lines

			// LOG_info("line: %s\n", line);

			char* path = line;
			char* alias = NULL;
			char* tmp = strchr(line,'\t');
			if (tmp) {
				tmp[0] = '\0';
				alias = tmp+1;
			}

			char sd_path[256];
			sprintf(sd_path, "%s%s", SDCARD_PATH, path);
			if (exists(sd_path)) {
				if (recents->count<MAX_RECENTS) {
					// LOG_info("path:%s alias:%s\n", path, alias);

					Recent* recent = Recent_new(path, alias);
					if (recent->available) has += 1;
					Array_push(recents, recent);
				}
			}
		}
		fclose(file);
	}

	saveRecents();

	return has>0;
}
static int hasCollections(void) {
	int has = 0;
	if (!exists(COLLECTIONS_PATH)) return has;

	DIR *dh = opendir(COLLECTIONS_PATH);
	struct dirent *dp;
	while((dp = readdir(dh)) != NULL) {
		if (hide(dp->d_name)) continue;
		has = 1;
		break;
	}
	closedir(dh);
	return has;
}
static int hasTools(void) {
	char tools_path[256];
    snprintf(tools_path, sizeof(tools_path), "%s/Tools/%s", SDCARD_PATH, PLATFORM);
	return exists(tools_path);
}

static Array* getCollections(void)
{
	DIR* dh = opendir(COLLECTIONS_PATH);
	if (dh) {
		struct dirent* dp;
		char full_path[256];
		snprintf(full_path, sizeof(full_path), "%s/", COLLECTIONS_PATH);
		char* tmp = full_path + strlen(full_path);

		Array* collections = Array_new();
		while ((dp = readdir(dh)) != NULL) {
			if (hide(dp->d_name)) continue;
			strcpy(tmp, dp->d_name);
			Array_push(collections, Entry_new(full_path, ENTRY_DIR)); // Collections are fake directories
		}
		closedir(dh); // Close immediately after use
		EntryArray_sort(collections);
		return collections;
	}
	return NULL;
}

static int isLaunchablePak(const char* path) {
	char launch_path[256];
	int len = snprintf(launch_path, sizeof(launch_path), "%s/launch.sh", path);
	return len>0 && (size_t)len<sizeof(launch_path) && exists(launch_path);
}

static void addPakEntries(Array* entries, const char* path) {
	DIR* dh = opendir(path);
	if (!dh) return;

	struct dirent* dp;
	char full_path[256];
	while ((dp = readdir(dh)) != NULL) {
		if (hide(dp->d_name)) continue;
		if (!suffixMatch(".pak", dp->d_name)) continue;
		if (dp->d_type!=DT_DIR && dp->d_type!=DT_UNKNOWN) continue;

		int len = snprintf(full_path, sizeof(full_path), "%s/%s", path, dp->d_name);
		if (len<=0 || (size_t)len>=sizeof(full_path)) continue;
		if (!isLaunchablePak(full_path)) continue;

		// Keep B1's metadata/version scan for paks first seen from the app root.
		pak_manifest_note_scan(full_path, SHARED_USERDATA_PATH "/.paks");
		Array_push(entries, Entry_new(full_path, ENTRY_PAK));
	}
	closedir(dh);
}

static Array* getApps(void) {
	Array* entries = Array_new();

	// Tools/$PLATFORM is the canonical app install root: installed apps and
	// their .pakz payloads both land there. It is not a game-only folder, so
	// show-tools no longer hides installed apps.
	if (!simple_mode) {
		char tools_path[256];
		snprintf(tools_path, sizeof(tools_path), "%s/Tools/%s", SDCARD_PATH, PLATFORM);
		addPakEntries(entries, tools_path);
	}

	EntryArray_sort(entries);

	// The launcher has no way to tell two identical labels apart; keep the first.
	for (int i=1; i<entries->count; ) {
		Entry* prev = entries->items[i-1];
		Entry* entry = entries->items[i];
		if (exactMatch(prev->name, entry->name)) {
			Entry_free(entry);
			Array_remove(entries, entry);
		}
		else i++;
	}

	return entries;
}

static Array* getQuickEntries(void) {
	Array* entries = Array_new();

	// We assume Menu_init was already called and populated this
	if (recents && recents->count)
		Array_push(entries, Entry_newNamed(FAUX_RECENT_PATH, ENTRY_DIR, TR("Recents")));

	if (hasCollections())
		Array_push(entries, Entry_newNamed(COLLECTIONS_PATH, ENTRY_DIR, TR("Collections")));

	// Games is no longer a default navigation category. Old game content stays
	// reachable through Collections, Recents, or an app that browses files.
	// Tools remains as a generic tools-folder shortcut; installed apps are
	// already listed flat at the root.
    if (hasTools() && CFG_getShowTools() && !simple_mode) {
		char tools_path[256];
		snprintf(tools_path, sizeof(tools_path), "%s/Tools/%s", SDCARD_PATH, PLATFORM);
		Array_push(entries, Entry_newNamed(tools_path, ENTRY_DIR, TR("Tools")));
    }

	// Keep the quick switcher non-empty on a card with no recents, collections
	// or tools; an empty Recents view is still a valid place to land.
	if (entries->count==0)
		Array_push(entries, Entry_newNamed(FAUX_RECENT_PATH, ENTRY_DIR, TR("Recents")));

	return entries;
}

static Array* getQuickToggles(void) {
	Array *entries = Array_new();

	Entry *settings = entryFromPakName("Settings");
	if (settings)
		Array_push(entries, settings);

	// quick actions
	if(WIFI_supported())
		Array_push(entries, Entry_new("Wifi", ENTRY_DIP));
	if(BT_supported())
		Array_push(entries, Entry_new("Bluetooth", ENTRY_DIP));
	if(PLAT_supportsDeepSleep() && !simple_mode)
		Array_push(entries, Entry_new("Sleep", ENTRY_DIP));
	Array_push(entries, Entry_new("Reboot", ENTRY_DIP));
	Array_push(entries, Entry_new("Poweroff", ENTRY_DIP));

	return entries;
}

static Array* getRoot(void) {
    Array* root = Array_new();

    if (hasRecents() && CFG_getShowRecents())
		Array_push(root, Entry_newNamed(FAUX_RECENT_PATH, ENTRY_DIR, TR("Recents")));

	Array *entries = getApps();

	// Handle collections
	if (hasCollections() && CFG_getShowCollections()) {
        if (entries->count) {
            Array_push(root, Entry_newNamed(COLLECTIONS_PATH, ENTRY_DIR, TR("Collections")));
        } else { // No visible apps, promote collections to root
			Array *collections = getCollections();
			Array_yoink(entries, collections);
        }
    }

    // Move entries to root
	Array_yoink(root, entries);

    return root;
}

static Entry* entryFromRecent(Recent* recent)
{
	if(!recent || !recent->available)
		return NULL;

	char sd_path[256];
	sprintf(sd_path, "%s%s", SDCARD_PATH, recent->path);
	if (!suffixMatch(".pak", sd_path)) return NULL; // roms are no longer launchable
	Entry* entry = Entry_new(sd_path, ENTRY_PAK);
	if (recent->alias) {
		free(entry->name);
		entry->name = strdup(recent->alias);
	}
	return entry;
}

static Array* getRecents(void) {
	Array* entries = Array_new();
	for (int i=0; i<recents->count; i++) {
		Recent *recent = recents->items[i];
		Entry *entry = entryFromRecent(recent);
		if(entry)
			Array_push(entries, entry);
	}
	return entries;
}

static Array* getCollection(char* path) {
	Array* entries = Array_new();
	FILE* file = fopen(path, "r");
	if (file) {
		char line[256];
		while (fgets(line,256,file)!=NULL) {
			normalizeNewline(line);
			trimTrailingNewlines(line);
			if (strlen(line)==0) continue; // skip empty lines

			char sd_path[256];
			sprintf(sd_path, "%s%s", SDCARD_PATH, line);
			if (exists(sd_path) && suffixMatch(".pak", sd_path)) {
				Array_push(entries, Entry_new(sd_path, ENTRY_PAK));
			}
		}
		fclose(file);
	}
	return entries;
}
static void addEntries(Array* entries, char* path) {
	DIR *dh = opendir(path);
	if (dh!=NULL) {
		struct dirent *dp;
		char* tmp;
		char full_path[256];
		sprintf(full_path, "%s/", path);
		tmp = full_path + strlen(full_path);
		while((dp = readdir(dh)) != NULL) {
			if (hide(dp->d_name)) continue;
			strcpy(tmp, dp->d_name);
			int is_dir = dp->d_type==DT_DIR;
			int type;
			if (is_dir) {
				// TODO: this should make sure launch.sh exists
				if (suffixMatch(".pak", dp->d_name)) {
					type = ENTRY_PAK;
				}
				else {
					type = ENTRY_DIR;
				}
			}
			else {
				type = ENTRY_DIR; // files are only reachable inside Collections
			}
			// B1: read pak.json when present and record the installed version.
			// Legacy launch.sh-only paks are untouched; the display name stays
			// the directory name (no icon/menu-root work in this step).
			if (type==ENTRY_PAK) pak_manifest_note_scan(full_path, SHARED_USERDATA_PATH "/.paks");
			Array_push(entries, Entry_new(full_path, type));
		}
		closedir(dh);
	}
}

static Array* getEntries(char* path){
	Array* entries = Array_new();

	addEntries(entries, path);

	EntryArray_sort(entries);
	return entries;
}

///////////////////////////////////////

static void queueNext(char* cmd) {
	LOG_info("cmd: %s\n", cmd);
	putFile("/tmp/next", cmd);
	quit = 1;
}

// based on https://stackoverflow.com/a/31775567/145965
static int replaceString(char *line, const char *search, const char *replace) {
   char *sp; // start of pattern
   if ((sp = strstr(line, search)) == NULL) {
      return 0;
   }
   int count = 1;
   int sLen = strlen(search);
   int rLen = strlen(replace);
   if (sLen > rLen) {
      // move from right to left
      char *src = sp + sLen;
      char *dst = sp + rLen;
      while((*dst = *src) != '\0') { dst++; src++; }
   } else if (sLen < rLen) {
      // move from left to right
      int tLen = strlen(sp) - sLen;
      char *stop = sp + rLen;
      char *src = sp + sLen + tLen;
      char *dst = sp + rLen + tLen;
      while(dst >= stop) { *dst = *src; dst--; src--; }
   }
   memcpy(sp, replace, rLen);
   count += replaceString(sp + rLen, search, replace);
   return count;
}
static char* escapeSingleQuotes(char* str) {
	// why not call replaceString directly?
	// call points require the modified string be returned
	// but replaceString is recursive and depends on its
	// own return value (but does it need to?)
	replaceString(str, "'", "'\\''");
	return str;
}

///////////////////////////////////////

// B4: pak resume is deliberately separate from the legacy ROM save slots.
// A pak is resumable only when pak.json declares the exact "saves" capability
// and B3 validates every payload file; a declaration alone is never enough.
//
// The launcher never writes state itself, and an app save returns through a
// fresh launcher process, so caching the last inspected pak cannot hide a new
// save. This avoids re-hashing the same payload when the switcher redraws or
// the A-button path inspects the entry already shown on screen.
static void readyPakResume(Entry* entry) {
	pak_can_resume = 0;
	if (!entry || entry->type!=ENTRY_PAK) {
		pak_resume_cache_valid = 0;
		return;
	}
	if (!(entry->capabilities & PAK_CAPABILITY_SAVES)) {
		pak_resume_cache_valid = 0;
		return;
	}
	if (pak_resume_cache_valid && exactMatch(pak_resume_cache_path, entry->path)) {
		pak_can_resume = pak_resume_cache_result;
		return;
	}

	pak_resume_cache_valid = 0;
	PakManifest manifest;
	if (pak_manifest_load(entry->path, &manifest)!=PAK_MANIFEST_OK) return;

	AppCapabilityInfo info;
	app_capabilities_inspect(SHARED_USERDATA_PATH "/.minui/apps", entry->path,
	                         &manifest, &info);
	pak_can_resume = (app_capabilities_actions(&info, APP_CAPABILITIES_DESKTOP, NULL)
	                  & APP_CAPABILITIES_ACTION_RESUME)!=0;

	strncpy(pak_resume_cache_path, entry->path, sizeof(pak_resume_cache_path)-1);
	pak_resume_cache_path[sizeof(pak_resume_cache_path)-1] = '\0';
	pak_resume_cache_result = pak_can_resume;
	pak_resume_cache_valid = 1;
}

static void readyEntryResume(Entry* entry) {
	readyPakResume(entry);
}

static void saveLast(char* path);
static void loadLast(void);

static void openPakMode(char* path, AppLaunchMode mode) {
	addRecent(path, NULL); // apps share the recents list with ROMs
	saveLast(path);

	// State-capable apps receive NEXTUI_APP_* below. Legacy paks keep the exact
	// old launch command, and a state setup failure must never block launching.
	PakManifest manifest;
	char cmd[APP_STATE_CMD_MAX];
	pak_manifest_load(path, &manifest);

	int built = app_state_build_launch(SHARED_USERDATA_PATH "/.minui/apps", path,
	                                   &manifest, mode, cmd, sizeof cmd);
	if (!built && mode==APP_LAUNCH_RESUME) {
		// The state may have changed between the menu check and the launch.
		// Falling back to a normal launch is safer than blocking the app.
		built = app_state_build_launch(SHARED_USERDATA_PATH "/.minui/apps", path,
		                               &manifest, APP_LAUNCH_NORMAL, cmd, sizeof cmd);
	}
	if (!built) {
		// NOTE: escapeSingleQuotes() modifies the passed string
		snprintf(cmd, sizeof cmd, "'%s/launch.sh'", escapeSingleQuotes(path));
	}
	queueNext(cmd);
}
static void openPak(char* path) {
	openPakMode(path, APP_LAUNCH_NORMAL);
}

// Launcher half of the FN action boundary: only a validated Tools/<PLATFORM>
// path reaches this handler. fnActionDispatch() owns parsing and scope checks.
static int launchPakFnAction(void* user, const char* rel) {
	(void)user;
	if (!rel || !rel[0]) return 0;

	char pak_path[256];
	snprintf(pak_path, sizeof(pak_path), "%s/Tools/%s/%s", SDCARD_PATH, PLATFORM, rel);

	char launch_path[256];
	snprintf(launch_path, sizeof(launch_path), "%s/launch.sh", pak_path);
	if (!exists(launch_path)) return 0; // stale binding, eg. the pak was deleted

	// unlike openPak() we save where the user *is*, not the pak itself, so exiting
	// the pak comes back to the same spot in the list
	if (top && top->entries->count>0) {
		Entry* entry = top->entries->items[top->selected];
		saveLast(entry->path);
	}

	char cmd[APP_STATE_CMD_MAX];
	PakManifest manifest;
	pak_manifest_load(pak_path, &manifest);
	if (!app_state_build_launch(SHARED_USERDATA_PATH "/.minui/apps", pak_path, &manifest,
	                            APP_LAUNCH_NORMAL, cmd, sizeof cmd)) {
		// NOTE: escapeSingleQuotes() modifies pak_path in place
		snprintf(cmd, sizeof cmd, "'%s/launch.sh'", escapeSingleQuotes(pak_path));
	}
	queueNext(cmd);
	return 1;
}

static UIOverlay* home_overlay;
static int home_overlay_dirty;

static int home_overlay_wifi_get(void* user) {
	(void)user;
	if (!WIFI_supported()) return -1;
	return WIFI_enabled() ? 1 : 0;
}
static int home_overlay_wifi_set(void* user, int on) {
	(void)user;
	WIFI_enable(on ? 1 : 0);
	return WIFI_enabled() == (on ? 1 : 0);
}
static int home_overlay_bluetooth_get(void* user) {
	(void)user;
	if (!BT_supported()) return -1;
	return BT_enabled() ? 1 : 0;
}
static int home_overlay_bluetooth_set(void* user, int on) {
	(void)user;
	BT_enable(on ? 1 : 0);
	return BT_enabled() == (on ? 1 : 0);
}
static int home_overlay_device_available(void* user) {
	(void)user;
	return 1;
}
static void home_overlay_input_reset(void* user) {
	(void)user;
	PAD_reset();
}

/* The launcher and adapted apps use the same shared overlay module. On the
 * launcher this owns the panel directly; adapted apps create their own
 * FnBridge/UIOverlay instance in their process. No pixels are injected into an
 * unadapted app. */
static void home_overlay_init(void) {
	static const UIOverlayDeviceHooks device = {
		.user = NULL,
		.available = home_overlay_device_available,
		.wifi_get = home_overlay_wifi_get,
		.wifi_set = home_overlay_wifi_set,
		.bluetooth_get = home_overlay_bluetooth_get,
		.bluetooth_set = home_overlay_bluetooth_set,
	};
	OverlayAppHooks hooks;

	memset(&hooks, 0, sizeof hooks);
	hooks.app_name = "NextUI";
	hooks.device = &device;
	hooks.input_reset = home_overlay_input_reset;
	if (WIFI_supported()) hooks.capabilities |= UI_OVERLAY_CAP_WIFI;
	if (BT_supported()) hooks.capabilities |= UI_OVERLAY_CAP_BLUETOOTH;

	home_overlay = UIOverlay_create(&hooks, NULL);
}

static int home_overlay_toggle(void) {
	if (!home_overlay) return 0;
	if (UIOverlay_isOpen(home_overlay))
		UIOverlay_close(home_overlay);
	else
		UIOverlay_open(home_overlay);
	home_overlay_dirty = 1;
	return 1;
}

// Run the action bound to a user-assignable button (0 == FN1, 1 == FN2,
// 2 == FN3/HOME). Returns 1 if the action was handled.
//
// The launcher owns `pak:` and `overlay` actions. `home` is an application
// action: there is no foreground app here, so it remains a deliberate no-op.
// FN2 keeps its built-in home default, and side HOME opens the shared overlay.
static int runFnAction(int index) {
	const char* action = CFG_getEffectiveFnAction(index);

	if (fnActionParse(action, NULL) == FN_ACTION_KIND_OVERLAY)
		return home_overlay_toggle();

	FnActionHandlers handlers = {
		.user = NULL,
		.launch_pak = launchPakFnAction,
		.exit_to_home = NULL,
		.open_overlay = NULL,
	};
	return fnActionDispatch(action, FN_ACTION_SCOPE_DESKTOP, &handlers)
		   == FN_ACTION_DISPATCH_HANDLED;
}
static bool isDirectSubdirectory(const Directory* parent, const char* child_path) {
    const char* parent_path = parent->path;

    size_t parent_len = strlen(parent_path);
    size_t child_len = strlen(child_path);

    // Child must be longer than parent to be a subdirectory
    if (child_len <= parent_len || strncmp(child_path, parent_path, parent_len) != 0) {
        return false;
    }

    // Next char after parent path must be '/'
    if (child_path[parent_len] != '/') return false;

    // Walk through the child path after parent, skipping PLATFORM segments
    const char* cursor = child_path + parent_len + 1; // skip the slash

    int levels = 0;
    while (*cursor) {
        const char* next = strchr(cursor, '/');
        size_t segment_len = next ? (size_t)(next - cursor) : strlen(cursor);

        if (segment_len == 0) break;

        // Copy segment into a buffer to compare
        char segment[PATH_MAX];
        if (segment_len >= PATH_MAX) return false;
        strncpy(segment, cursor, segment_len);
        segment[segment_len] = '\0';

        // Count level only if it's not PLATFORM
        if (strcmp(segment, PLATFORM) != 0 && strcmp(segment, "Roms") != 0) {
            levels++;
        }

        if (!next) break;
        cursor = next + 1;
    }

    return (levels == 1);  // exactly one meaningful level deeper
}

Array* pathToStack(const char* path) {
	Array* array = Array_new();

	if (!path || strlen(path) == 0) return array;

	if (!prefixMatch(SDCARD_PATH, path)) return array;

	// Always include root directory
	Directory* root_dir = Directory_new(SDCARD_PATH, 0);
	root_dir->start = 0;
	root_dir->end = (root_dir->entries->count < MAIN_ROW_COUNT) ? root_dir->entries->count : MAIN_ROW_COUNT;
	Array_push(array, root_dir);

	if (exactMatch(path, SDCARD_PATH)) return array;

	char temp_path[PATH_MAX];
	strcpy(temp_path, SDCARD_PATH);
	size_t current_len = strlen(SDCARD_PATH);

	const char* cursor = path + current_len;
	if (*cursor == '/') cursor++;

	while (*cursor) {
		const char* next = strchr(cursor, '/');
		size_t segment_len = next ? (size_t)(next - cursor) : strlen(cursor);
		if (segment_len == 0 || segment_len >= PATH_MAX) break;

		char segment[PATH_MAX];
		strncpy(segment, cursor, segment_len);
		segment[segment_len] = '\0';

		// Append '/' if needed
		if (temp_path[current_len - 1] != '/') {
			if (current_len + 1 >= PATH_MAX) break;
			temp_path[current_len++] = '/';
			temp_path[current_len] = '\0';
		}

		// Append segment
		if (current_len + segment_len >= PATH_MAX) break;
		strcat(temp_path, segment);
		current_len += segment_len;

		if (strcmp(segment, PLATFORM) == 0) {
			// Merge with previous directory
			if (array->count > 0) {
				// Remove the previous directory
				Directory* last = (Directory*)array->items[array->count - 1];
				Array_pop(array);
				Directory_free(last); // assuming you have a Directory_free

				// Replace with updated one using combined path
				Directory* merged = Directory_new(temp_path, 0);
				merged->start = 0;
				merged->end = (merged->entries->count < MAIN_ROW_COUNT) ? merged->entries->count : MAIN_ROW_COUNT;
				Array_push(array, merged);
			}
		} else {
			Directory* dir = Directory_new(temp_path, 0);
			dir->start = 0;
			dir->end = (dir->entries->count < MAIN_ROW_COUNT) ? dir->entries->count : MAIN_ROW_COUNT;
			Array_push(array, dir);
		}

		if (!next) break;
		cursor = next + 1;
	}

	return array;
}

static void openDirectory(char* path) {
	// If this is the exact same directory for some reason, just return.
	if(top && strcmp(top->path, path) == 0)
		return;

	// If this path is a direct subdirectory of top, push it on top of the stack
	// If it isnt, we need to recreate the stack to keep navigation consistent
	if(!top || isDirectSubdirectory(top, path)) {
		int selected = 0;
		int start = 0;
		int end = 0;
		if (top && top->entries->count>0) {
			if (restore_depth==stack->count && top->selected==restore_relative) {
				selected = restore_selected;
				start = restore_start;
				end = restore_end;
			}
		}

		top = Directory_new(path, selected);
		top->start = start;
		top->end = end ? end : ((top->entries->count<MAIN_ROW_COUNT) ? top->entries->count : MAIN_ROW_COUNT);

		Array_push(stack, top);
	}
	else {
		// keep a copy of path, which might be a reference into stack which is about to be freed
		char temp_path[256];
		strcpy(temp_path, path);

		// construct a fresh stack by walking upwards until SDCARD_ROOT
		DirectoryArray_free(stack);

		stack = pathToStack(temp_path);
		top = stack->items[stack->count - 1];
	}
}

static void closeDirectory(void) {
	restore_selected = top->selected;
	restore_start = top->start;
	restore_end = top->end;
	DirectoryArray_pop(stack);
	restore_depth = stack->count;
	top = stack->items[stack->count-1];
	restore_relative = top->selected;
}

static void cleanupImageLoaderPool(void); // Forward declaration

static void toggleQuick(Entry* self)
{
	if(!self)
		return;

	if(!strcmp(self->name, "Wifi")) {
		WIFI_enable(!WIFI_enabled());
	}
	else if(!strcmp(self->name, "Bluetooth")) {
		BT_enable(!BT_enabled());
	}
	else if(!strcmp(self->name, "Sleep")) {
		PWR_sleep();
	}
	else if(!strcmp(self->name, "Reboot")) {
		cleanupImageLoaderPool();  // Stop worker threads before SDL cleanup
		PWR_powerOff(1);
	}
	else if(!strcmp(self->name, "Poweroff")) {
		cleanupImageLoaderPool();  // Stop worker threads before SDL cleanup
		PWR_powerOff(0);
	}
}

static void Entry_open(Entry* self) {
	recent_alias = self->name;  // yiiikes
	if (self->type==ENTRY_PAK) {
		startgame = 1;
		int resume = should_resume_pak;
		should_resume_pak = 0;
		if (resume) openPakMode(self->path, APP_LAUNCH_RESUME);
		else openPak(self->path);
	}
	else if (self->type==ENTRY_DIR) {
		openDirectory(self->path);
	}
	else if (self->type==ENTRY_DIP) {
		toggleQuick(self);
	}
}

///////////////////////////////////////

static void saveLast(char* path) {
	// special case for recently played
	if (exactMatch(top->path, FAUX_RECENT_PATH)) {
		// NOTE: that we don't have to save the file because
		// your most recently played game will always be at
		// the top which is also the default selection
		path = FAUX_RECENT_PATH;
	}
	putFile(LAST_PATH, path);
}
static void loadLast(void) { // call after loading root directory
	if (!exists(LAST_PATH)) return;

	char last_path[256];
	getFile(LAST_PATH, last_path, 256);

	char full_path[256];
	strcpy(full_path, last_path);

	char* tmp;
	char filename[256];
	tmp = strrchr(last_path, '/');
	if (tmp) strcpy(filename, tmp);

	Array* last = Array_new();
	while (!exactMatch(last_path, SDCARD_PATH)) {
		Array_push(last, strdup(last_path));

		char* slash = strrchr(last_path, '/');
		last_path[(slash-last_path)] = '\0';
	}

	while (last->count>0) {
		char* path = Array_pop(last);
		{
			for (int i=0; i<top->entries->count; i++) {
				Entry* entry = top->entries->items[i];

				if (exactMatch(entry->path, path) || (prefixMatch(COLLECTIONS_PATH, full_path) && suffixMatch(filename, entry->path))) {
					top->selected = i;
					if (i>=top->end) {
						top->start = i;
						top->end = top->start + MAIN_ROW_COUNT;
						if (top->end>top->entries->count) {
							top->end = top->entries->count;
							top->start = top->end - MAIN_ROW_COUNT;
						}
					}
					if (last->count==0 && !exactMatch(entry->path, FAUX_RECENT_PATH) && !(!exactMatch(entry->path, COLLECTIONS_PATH) && prefixMatch(COLLECTIONS_PATH, entry->path))) break; // don't show contents of auto-launch dirs

					if (entry->type==ENTRY_DIR) {
						openDirectory(entry->path);
						break;
					}
				}
			}
		}
		free(path); // we took ownership when we popped it
	}

	StringArray_free(last);

	if (top->selected >= 0 && top->selected < top->entries->count) {
		Entry *selected_entry = top->entries->items[top->selected];
		readyEntryResume(selected_entry);
	}
}

///////////////////////////////////////

static void QuickMenu_init(void) {
	quick = getQuickEntries();
	quickActions = getQuickToggles();
}
static void QuickMenu_quit(void) {
	EntryArray_free(quick);
	EntryArray_free(quickActions);
}

static void Menu_init(void) {
	stack = Array_new(); // array of open Directories
	recents = Array_new();

	openDirectory(SDCARD_PATH);
	loadLast(); // restore state when available

	QuickMenu_init(); // needs Menu_init
}
static void Menu_quit(void) {
	RecentArray_free(recents);
	DirectoryArray_free(stack);

	QuickMenu_quit();
}

///////////////////////////////////////

static int dirty = 1;
static int previous_row = 0;
static int previous_depth = 0;

///////////////////////////////////////

enum {
	ANIM_NONE = 0,
	SLIDE_LEFT = 1,
	SLIDE_RIGHT = 2,
};

typedef void (*BackgroundLoadedCallback)(SDL_Surface* surface);

typedef struct {
    char imagePath[MAX_PATH];
    BackgroundLoadedCallback callback;
    void* userData;
} LoadBackgroundTask;

typedef struct finishedTask {
	int startX;
	int targetX;
	int startY;
	int targetY;
	int targetTextY;
	int move_y;
	int move_w;
	int move_h;
	int frames;
	int done;
	void* userData;
	  char *entry_name;
	SDL_Rect dst;
} finishedTask;

typedef void (*AnimTaskCallback)(finishedTask *task);
typedef struct AnimTask {
	int startX;
	int targetX;
	int startY;
	int targetY;
	int targetTextY;
	int move_w;
	int move_h;
	int frames;
	AnimTaskCallback callback;
	void* userData;
	 char *entry_name;
	SDL_Rect dst;
} AnimTask;

typedef struct TaskNode {
    LoadBackgroundTask* task;
    struct TaskNode* next;
} TaskNode;
typedef struct AnimTaskNode {
	AnimTask* task;
    struct AnimTaskNode* next;
} AnimTaskNode;

static TaskNode* taskBGQueueHead = NULL;
static TaskNode* taskBGQueueTail = NULL;
static TaskNode* taskThumbQueueHead = NULL;
static TaskNode* taskThumbQueueTail = NULL;
static AnimTaskNode* animTaskQueueHead = NULL;
static AnimTaskNode* animTtaskQueueTail = NULL;
static SDL_mutex* bgqueueMutex = NULL;
static SDL_mutex* thumbqueueMutex = NULL;
static SDL_mutex* animqueueMutex = NULL;
static SDL_cond* bgqueueCond = NULL;
static SDL_cond* thumbqueueCond = NULL;
static SDL_cond* animqueueCond = NULL;

static SDL_mutex* bgMutex = NULL;
static SDL_mutex* thumbMutex = NULL;
static SDL_mutex* animMutex = NULL;
static SDL_mutex* frameMutex = NULL;
static SDL_mutex* fontMutex = NULL;
static SDL_cond* flipCond = NULL;

static SDL_Thread* bgLoadThread = NULL;
static SDL_Thread* thumbLoadThread = NULL;
static SDL_Thread* animWorkerThread = NULL;

static SDL_atomic_t workerThreadsShutdown; // Flag to signal threads to exit (atomic for thread safety)

static SDL_Surface* folderbgbmp = NULL;
static SDL_Surface* thumbbmp = NULL;
static SDL_Surface* screen = NULL; // Must be assigned externally
static SDL_Surface* globalpill = NULL;
static SDL_Surface* globalText = NULL;

static int had_thumb = 0;
static int ox;
static int oy;
static SDL_atomic_t animationDrawAtomic;
static SDL_atomic_t needDrawAtomic;

static inline void setAnimationDraw(int v) { SDL_AtomicSet(&animationDrawAtomic, v); }
static inline int getAnimationDraw(void) { return SDL_AtomicGet(&animationDrawAtomic); }
static inline void setNeedDraw(int v) { SDL_AtomicSet(&needDrawAtomic, v); }
static inline int getNeedDraw(void) { return SDL_AtomicGet(&needDrawAtomic); }

static void updatePillTextSurface(const char* entry_name, int move_w, SDL_Color text_color) {
	int crop_w = move_w - SCALE1(BUTTON_PADDING * 2);
	if (crop_w <= 0) return;

	SDL_LockMutex(fontMutex);
	SDL_Surface* tmp = TTF_RenderUTF8_Blended(font.large, entry_name, text_color);
	SDL_UnlockMutex(fontMutex);
	if (!tmp) return;

	SDL_Surface* converted = SDL_ConvertSurfaceFormat(tmp, screen->format->format, 0);
	SDL_FreeSurface(tmp);
	if (!converted) return;

	SDL_Rect crop_rect = { 0, 0, crop_w, converted->h };
	SDL_Surface* cropped = SDL_CreateRGBSurfaceWithFormat(
		0, crop_rect.w, crop_rect.h, screen->format->BitsPerPixel, screen->format->format
	);
	if (cropped) {
		SDL_BlitSurface(converted, &crop_rect, cropped, NULL);
	}
	SDL_FreeSurface(converted);
	if (!cropped) return;

	SDL_LockMutex(animMutex);
	if (globalText) SDL_FreeSurface(globalText);
	globalText = cropped;
	SDL_UnlockMutex(animMutex);
}
int folderbgchanged=0;
int thumbchanged=0;

// queue a new image load task :D
#define MAX_QUEUE_SIZE 1

int currentBGQueueSize = 0;
int currentThumbQueueSize = 0;
int currentAnimQueueSize = 0;

void enqueueBGTask(LoadBackgroundTask* task) {
	SDL_LockMutex(bgqueueMutex);
    TaskNode* node = (TaskNode*)malloc(sizeof(TaskNode));
    node->task = task;
    node->next = NULL;

    // If queue is full, drop the oldest task (head)
    if (currentBGQueueSize >= MAX_QUEUE_SIZE) {
        TaskNode* oldNode = taskBGQueueHead;
        if (oldNode) {
            taskBGQueueHead = oldNode->next;
            if (!taskBGQueueHead) {
                taskBGQueueTail = NULL;
            }
            if (oldNode->task) {
                free(oldNode->task);  // Only if task was malloc'd
            }
            free(oldNode);
            currentBGQueueSize--;
        }
    }

    // Enqueue the new task
    if (taskBGQueueTail) {
        taskBGQueueTail->next = node;
        taskBGQueueTail = node;
    } else {
        taskBGQueueHead = taskBGQueueTail = node;
    }

    currentBGQueueSize++;
    SDL_CondSignal(bgqueueCond);
    SDL_UnlockMutex(bgqueueMutex);
}
void enqueueThumbTask(LoadBackgroundTask* task) {
	SDL_LockMutex(thumbqueueMutex);
    TaskNode* node = (TaskNode*)malloc(sizeof(TaskNode));
    node->task = task;
    node->next = NULL;

    // If queue is full, drop the oldest task (head)
    if (currentThumbQueueSize >= MAX_QUEUE_SIZE) {
        TaskNode* oldNode = taskThumbQueueHead;
        if (oldNode) {
            taskThumbQueueHead = oldNode->next;
            if (!taskThumbQueueHead) {
                taskThumbQueueTail = NULL;
            }
            if (oldNode->task) {
                free(oldNode->task);  // Only if task was malloc'd
            }
            free(oldNode);
            currentThumbQueueSize--;
        }
    }

    // Enqueue the new task
    if (taskThumbQueueTail) {
        taskThumbQueueTail->next = node;
        taskThumbQueueTail = node;
    } else {
        taskThumbQueueHead = taskThumbQueueTail = node;
    }

    currentThumbQueueSize++;
    SDL_CondSignal(thumbqueueCond);
    SDL_UnlockMutex(thumbqueueMutex);
}

// Worker thread
int BGLoadWorker(void* unused) {
    while (!SDL_AtomicGet(&workerThreadsShutdown)) {
        SDL_LockMutex(bgqueueMutex);
        while (!taskBGQueueHead && !SDL_AtomicGet(&workerThreadsShutdown)) {
        	SDL_CondWait(bgqueueCond, bgqueueMutex);
        }
        if (SDL_AtomicGet(&workerThreadsShutdown)) {
            SDL_UnlockMutex(bgqueueMutex);
            break;
        }
        TaskNode* node = taskBGQueueHead;
        taskBGQueueHead = node->next;
        if (!taskBGQueueHead) taskBGQueueTail = NULL;
        SDL_UnlockMutex(bgqueueMutex);
		// give processor lil space in between queue items for other shit
		//SDL_Delay(100);
        LoadBackgroundTask* task = node->task;
        free(node);

        SDL_Surface* result = NULL;
        if (access(task->imagePath, F_OK) == 0) {
            SDL_Surface* image = IMG_Load(task->imagePath);
            if (image) {
                SDL_Surface* imageRGBA = SDL_ConvertSurfaceFormat(image, screen->format->format, 0);
                SDL_FreeSurface(image);
                result = imageRGBA;
            }
        }

        if (task->callback) {
			task->callback(result);
		}
        free(task);
		SDL_LockMutex(bgqueueMutex);
		if (!taskBGQueueHead) taskBGQueueTail = NULL;
		currentBGQueueSize--;  // <-- add this
		SDL_UnlockMutex(bgqueueMutex);
    }
    return 0;
}
int ThumbLoadWorker(void* unused) {
    while (!SDL_AtomicGet(&workerThreadsShutdown)) {
        SDL_LockMutex(thumbqueueMutex);
        while (!taskThumbQueueHead && !SDL_AtomicGet(&workerThreadsShutdown)) {
        	SDL_CondWait(thumbqueueCond, thumbqueueMutex);
        }
        if (SDL_AtomicGet(&workerThreadsShutdown)) {
            SDL_UnlockMutex(thumbqueueMutex);
            break;
        }
        TaskNode* node = taskThumbQueueHead;
        taskThumbQueueHead = node->next;
        if (!taskThumbQueueHead) taskThumbQueueTail = NULL;
        SDL_UnlockMutex(thumbqueueMutex);
		// give processor lil space in between queue items for other shit
		//SDL_Delay(100);
        LoadBackgroundTask* task = node->task;
        free(node);

        SDL_Surface* result = NULL;
        if (access(task->imagePath, F_OK) == 0) {
            SDL_Surface* image = IMG_Load(task->imagePath);
            if (image) {
                SDL_Surface* imageRGBA = SDL_ConvertSurfaceFormat(image, screen->format->format, 0);
                SDL_FreeSurface(image);
                result = imageRGBA;
            }
        }

        if (task->callback) {
			task->callback(result);
		}
        free(task);
		SDL_LockMutex(thumbqueueMutex);
		if (!taskThumbQueueHead) taskThumbQueueTail = NULL;
		currentThumbQueueSize--;  // <-- add this
		SDL_UnlockMutex(thumbqueueMutex);
    }
    return 0;
}

void startLoadFolderBackground(const char* imagePath, BackgroundLoadedCallback callback, void* userData) {
    LoadBackgroundTask* task = malloc(sizeof(LoadBackgroundTask));
    if (!task) return;

 	snprintf(task->imagePath, sizeof(task->imagePath), "%s", imagePath);
    task->callback = callback;
    task->userData = userData;
    enqueueBGTask(task);
}

void onBackgroundLoaded(SDL_Surface* surface) {
	SDL_LockMutex(bgMutex);
	folderbgchanged = 1;
	if (folderbgbmp) SDL_FreeSurface(folderbgbmp);
    if (!surface) {
		folderbgbmp = NULL;
		SDL_UnlockMutex(bgMutex);
		return;
	}
    folderbgbmp = surface;
	setNeedDraw(1);
	SDL_UnlockMutex(bgMutex);
}

void startLoadThumb(const char* thumbpath, BackgroundLoadedCallback callback, void* userData) {
    LoadBackgroundTask* task = malloc(sizeof(LoadBackgroundTask));
    if (!task) return;

    snprintf(task->imagePath, sizeof(task->imagePath), "%s", thumbpath);
    task->callback = callback;
    task->userData = userData;
    enqueueThumbTask(task);
}
void onThumbLoaded(SDL_Surface* surface) {
	SDL_LockMutex(thumbMutex);
	thumbchanged = 1;
	if (thumbbmp) SDL_FreeSurface(thumbbmp);
    if (!surface) {
		thumbbmp = NULL;
		SDL_UnlockMutex(thumbMutex);
		return;
	}


    thumbbmp = surface;
	int img_w = thumbbmp->w;
	int img_h = thumbbmp->h;
	double aspect_ratio = (double)img_h / img_w;
	int max_w = (int)(screen->w * CFG_getGameArtWidth());
	int max_h = (int)(screen->h * 0.6);
	int new_w = max_w;
	int new_h = (int)(new_w * aspect_ratio);

	if (new_h > max_h) {
		new_h = max_h;
		new_w = (int)(new_h / aspect_ratio);
	}

	GFX_ApplyRoundedCorners_8888(
		thumbbmp,
		&(SDL_Rect){0, 0, thumbbmp->w, thumbbmp->h},
		SCALE1((float)CFG_getThumbnailRadius() * ((float)img_w / (float)new_w))
	);
	setNeedDraw(1);
	SDL_UnlockMutex(thumbMutex);
}

SDL_Rect pillRect;
int pilltargetY =0;
int pilltargetTextY =0;
void animcallback(finishedTask *task) {
	SDL_LockMutex(animMutex);
	pillRect = task->dst;
	if(pillRect.w > 0 && pillRect.h > 0) {
		pilltargetY = +screen->w; // move offscreen
		if(task->done) {
			pilltargetY = task->targetY;
			pilltargetTextY = task->targetTextY;
		}
		setNeedDraw(1);
	}
	SDL_UnlockMutex(animMutex);
	setAnimationDraw(1);
}
bool frameReady = true;
bool pillanimdone = false;

int animWorker(void* unused) {
	  while (!SDL_AtomicGet(&workerThreadsShutdown)) {
 		SDL_LockMutex(animqueueMutex);
    while (!animTaskQueueHead && !SDL_AtomicGet(&workerThreadsShutdown)) {
        SDL_CondWait(animqueueCond, animqueueMutex);
    }
    if (SDL_AtomicGet(&workerThreadsShutdown)) {
        SDL_UnlockMutex(animqueueMutex);
        break;
    }
    AnimTaskNode* node = animTaskQueueHead;
    animTaskQueueHead = node->next;
    if (!animTaskQueueHead) animTtaskQueueTail = NULL;
		SDL_UnlockMutex(animqueueMutex);

    AnimTask* task = node->task;
		finishedTask* finaltask = (finishedTask*)malloc(sizeof(finishedTask));
		int total_frames = task->frames;
		for (int frame = 0; frame <= total_frames; frame++) {
			// Check for shutdown at start of each frame
			if (SDL_AtomicGet(&workerThreadsShutdown)) break;

			float t = (float)frame / total_frames;
			if (t > 1.0f) t = 1.0f;

			int current_x = task->startX + (int)((task->targetX - task->startX) * t);
			int current_y = task->startY + (int)(( task->targetY -  task->startY) * t);

			SDL_Rect moveDst = { current_x, current_y, task->move_w, task->move_h };
			finaltask->dst = moveDst;
			finaltask->entry_name = task->entry_name;
			finaltask->move_w = task->move_w;
			finaltask->move_h = task->move_h;
			finaltask->targetY = task->targetY;
			finaltask->targetTextY = task->targetTextY;
			finaltask->move_y = SCALE1(PADDING + task->targetY) + (task->targetTextY - task->targetY);
			finaltask->done = 0;
			if(frame >= total_frames) finaltask->done=1;
			task->callback(finaltask);
			SDL_LockMutex(frameMutex);
			while (!frameReady && !SDL_AtomicGet(&workerThreadsShutdown)) {
				SDL_CondWait(flipCond, frameMutex);
			}
			frameReady = false;
			SDL_UnlockMutex(frameMutex);

		}
		SDL_LockMutex(animqueueMutex);
		if (!animTaskQueueHead) animTtaskQueueTail = NULL;
		currentAnimQueueSize--;  // <-- add this
		SDL_UnlockMutex(animqueueMutex);

		SDL_LockMutex(animMutex);
		pillanimdone = true;
		free(finaltask);
		SDL_UnlockMutex(animMutex);
	}
}

void enqueueanmimtask(AnimTask* task) {
    AnimTaskNode* node = (AnimTaskNode*)malloc(sizeof(AnimTaskNode));
    node->task = task;
    node->next = NULL;

    SDL_LockMutex(animqueueMutex);
	pillanimdone = false;
    // If queue is full, drop the oldest task (head)
    if (currentAnimQueueSize >= 1) {
        AnimTaskNode* oldNode = animTaskQueueHead;
        if (oldNode) {
            animTaskQueueHead = oldNode->next;
            if (!animTaskQueueHead) {
                animTtaskQueueTail = NULL;
            }
            if (oldNode->task) {
                free(oldNode->task);  // Only if task was malloc'd
            }
            free(oldNode);
            currentAnimQueueSize--;
        }
    }

    // Enqueue the new task
    if (animTtaskQueueTail) {
        animTtaskQueueTail->next = node;
        animTtaskQueueTail = node;
    } else {
        animTaskQueueHead = animTtaskQueueTail = node;
    }

    currentAnimQueueSize++;
    SDL_CondSignal(animqueueCond);
    SDL_UnlockMutex(animqueueMutex);
}

void animPill(AnimTask *task) {
	task->callback = animcallback;
	enqueueanmimtask(task);
}

void initImageLoaderPool() {
	// Initialize shutdown flag to 0
	SDL_AtomicSet(&workerThreadsShutdown, 0);
	SDL_AtomicSet(&animationDrawAtomic, 1);
	SDL_AtomicSet(&needDrawAtomic, 0);

    thumbqueueMutex = SDL_CreateMutex();
    bgqueueMutex = SDL_CreateMutex();
    bgqueueCond = SDL_CreateCond();
    thumbqueueCond = SDL_CreateCond();
	bgMutex = SDL_CreateMutex();
	thumbMutex = SDL_CreateMutex();
	animMutex = SDL_CreateMutex();
	animqueueMutex = SDL_CreateMutex();
	animqueueCond = SDL_CreateCond();
	frameMutex = SDL_CreateMutex();
	fontMutex = SDL_CreateMutex();
	flipCond = SDL_CreateCond();

    bgLoadThread = SDL_CreateThread(BGLoadWorker, "BGLoadWorker", NULL);
    thumbLoadThread = SDL_CreateThread(ThumbLoadWorker, "ThumbLoadWorker", NULL);
	animWorkerThread = SDL_CreateThread(animWorker, "animWorker", NULL);
}

void cleanupImageLoaderPool() {
	// Signal all worker threads to exit (atomic set for thread safety)
	SDL_AtomicSet(&workerThreadsShutdown, 1);

	// Wake up all waiting threads
	if (bgqueueCond) SDL_CondSignal(bgqueueCond);
	if (thumbqueueCond) SDL_CondSignal(thumbqueueCond);
	if (animqueueCond) SDL_CondSignal(animqueueCond);
	if (flipCond) SDL_CondSignal(flipCond);  // Wake up animWorker if stuck waiting for frame flip

	// Wait for all worker threads to finish
	if (bgLoadThread) {
		SDL_WaitThread(bgLoadThread, NULL);
		bgLoadThread = NULL;
	}
	if (thumbLoadThread) {
		SDL_WaitThread(thumbLoadThread, NULL);
		thumbLoadThread = NULL;
	}
	if (animWorkerThread) {
		SDL_WaitThread(animWorkerThread, NULL);
		animWorkerThread = NULL;
	}

	// Small delay to ensure llvmpipe/OpenGL threads have completed any pending operations
	SDL_Delay(10);

	// Acquire and release each mutex before destroying to ensure no thread is in a critical section
	// This creates a memory barrier and ensures proper synchronization
	if (bgqueueMutex) { SDL_LockMutex(bgqueueMutex); SDL_UnlockMutex(bgqueueMutex); }
	if (thumbqueueMutex) { SDL_LockMutex(thumbqueueMutex); SDL_UnlockMutex(thumbqueueMutex); }
	if (animqueueMutex) { SDL_LockMutex(animqueueMutex); SDL_UnlockMutex(animqueueMutex); }
	if (bgMutex) { SDL_LockMutex(bgMutex); SDL_UnlockMutex(bgMutex); }
	if (thumbMutex) { SDL_LockMutex(thumbMutex); SDL_UnlockMutex(thumbMutex); }
	if (animMutex) { SDL_LockMutex(animMutex); SDL_UnlockMutex(animMutex); }
	if (frameMutex) { SDL_LockMutex(frameMutex); SDL_UnlockMutex(frameMutex); }
	if (fontMutex) { SDL_LockMutex(fontMutex); SDL_UnlockMutex(fontMutex); }

	// Destroy mutexes and condition variables
	if (bgqueueMutex) SDL_DestroyMutex(bgqueueMutex);
	if (thumbqueueMutex) SDL_DestroyMutex(thumbqueueMutex);
	if (animqueueMutex) SDL_DestroyMutex(animqueueMutex);
	if (bgMutex) SDL_DestroyMutex(bgMutex);
	if (thumbMutex) SDL_DestroyMutex(thumbMutex);
	if (animMutex) SDL_DestroyMutex(animMutex);
	if (frameMutex) SDL_DestroyMutex(frameMutex);
	if (fontMutex) SDL_DestroyMutex(fontMutex);

	if (bgqueueCond) SDL_DestroyCond(bgqueueCond);
	if (thumbqueueCond) SDL_DestroyCond(thumbqueueCond);
	if (animqueueCond) SDL_DestroyCond(animqueueCond);
	if (flipCond) SDL_DestroyCond(flipCond);

	// Set pointers to NULL after destruction
	bgqueueMutex = NULL;
	thumbqueueMutex = NULL;
	animqueueMutex = NULL;
	bgMutex = NULL;
	thumbMutex = NULL;
	animMutex = NULL;
	frameMutex = NULL;
	fontMutex = NULL;
	bgqueueCond = NULL;
	thumbqueueCond = NULL;
	animqueueCond = NULL;
	flipCond = NULL;
}
///////////////////////////////////////

int main (int argc, char *argv[]) {
	// LOG_info("time from launch to:\n");
	// unsigned long main_begin = SDL_GetTicks();
	// unsigned long first_draw = 0;

	simple_mode = exists(SIMPLE_MODE_PATH);

	LOG_info("NextUI\n");
	InitSettings();

	screen = GFX_init(MODE_MAIN);
// LOG_info("- graphics init: %lu\n", SDL_GetTicks() - main_begin);

	PAD_init();
	home_overlay_init();
	// LOG_info("- input init: %lu\n", SDL_GetTicks() - main_begin);
	VIB_init();
	PWR_init();
	if (!HAS_POWER_BUTTON && !simple_mode) PWR_disableSleep();
	// LOG_info("- power init: %lu\n", SDL_GetTicks() - main_begin);

	// start my threaded image loader :D
	initImageLoaderPool();
	Menu_init();
	int qm_row = 0;
	int qm_col = 0;
	int qm_slot = 0;
	int qm_shift = 0;
	int qm_slots = QUICK_SWITCHER_COUNT > quick->count ? quick->count : QUICK_SWITCHER_COUNT;
	// LOG_info("- menu init: %lu\n", SDL_GetTicks() - main_begin);

	int lastScreen = SCREEN_OFF;
	int currentScreen = CFG_getDefaultView();

	if(exists(GAME_SWITCHER_PERSIST_PATH)) {
		// consider this "consumed", dont bring up the switcher next time we regularly exit a game
		unlink(GAME_SWITCHER_PERSIST_PATH);
		currentScreen = SCREEN_GAMESWITCHER;
	}

	// add a nice fade into the game switcher
	if(currentScreen == SCREEN_GAMESWITCHER)
		lastScreen = SCREEN_GAME;

	GFX_setVsync(VSYNC_STRICT);
	PWR_setCPUSpeed(CPU_SPEED_AUTO);

	PAD_reset();
	GFX_clearLayers(LAYER_ALL);
	GFX_clear(screen);

	int show_setting = 0; // 1=brightness,2=volume
	int was_online = PWR_isOnline();
    int had_bt = PLAT_btIsConnected();
	int had_sink = GetAudioSink();

	int selected_row = top->selected - top->start;
	float targetY;
	float previousY;
	int is_scrolling = 0;
	bool list_show_entry_names = true;

	char folderBgPath[1024];
	folderbgbmp = NULL;

	SDL_Surface * blackBG = SDL_CreateRGBSurfaceWithFormat(0,screen->w,screen->h,screen->format->BitsPerPixel,screen->format->format);
	SDL_FillRect(blackBG,NULL,SDL_MapRGBA(screen->format,0,0,0,255));

	SDL_LockMutex(animMutex);
	globalpill = SDL_CreateRGBSurfaceWithFormat(SDL_SWSURFACE, screen->w, SCALE1(PILL_SIZE), FIXED_DEPTH, screen->format->format);
	globalText = SDL_CreateRGBSurfaceWithFormat(SDL_SWSURFACE, screen->w, SCALE1(PILL_SIZE), FIXED_DEPTH, screen->format->format);
	static int globallpillW = 0;
	SDL_UnlockMutex(animMutex);

	//LOG_info("Start time time %ims\n",SDL_GetTicks());
	while (!quit) {
		GFX_startFrame();
		unsigned long now = SDL_GetTicks();

		PAD_poll();

		int selected = top->selected;
		int total = top->entries->count;

		PWR_update(&dirty, &show_setting, NULL, NULL);

		// user-assignable buttons, handled before the per-screen input so they work
		// from the game list, the game switcher and the quick menu alike.
		// skipped while the brightness/volume overlay owns input, same as L1/R1 below.
		if (!show_setting) {
			if (PAD_justPressed(BTN_FN1)) runFnAction(0);
			else if (PAD_justPressed(BTN_FN2)) runFnAction(1);
			else if (PAD_justPressed(BTN_FN3)) runFnAction(2);
		}

		int home_overlay_owns_input = 0;
		int gsanimdir = ANIM_NONE;
		if (home_overlay && UIOverlay_isOpen(home_overlay)) {
			UIOverlay_pollInput(home_overlay);
			home_overlay_owns_input = 1;
		}
		if (home_overlay_owns_input) {
			dirty = 1;
		} else {
		int is_online = PWR_isOnline();
		if (was_online!=is_online)
			dirty = 1;
		was_online = is_online;

        int has_bt = PLAT_btIsConnected();
        if (had_bt != has_bt)
            dirty = 1;
        had_bt = has_bt;

		int has_sink = GetAudioSink();
		if (had_sink != has_sink)
			dirty = 1;
		had_sink = has_sink;

		if (currentScreen == SCREEN_QUICKMENU) {
			int qm_total = qm_row == 0 ? quick->count : quickActions->count;

			if (PAD_justPressed(BTN_B) || PAD_tappedMenu(now)) {
				currentScreen = SCREEN_GAMELIST;
				folderbgchanged = 1; // The background painting code is a clusterfuck, just force a repaint here
				dirty = 1;
			}
			else if (PAD_justReleased(BTN_A)) {
				Entry *selected = qm_row == 0 ? quick->items[qm_col] : quickActions->items[qm_col];
				if(selected->type != ENTRY_DIP) {
					currentScreen = SCREEN_GAMELIST;
					total = top->entries->count;
					// prevent restoring list state, game list screen currently isnt our nav origin
					top->selected = 0;
					top->start = 0;
					top->end = top->start + MAIN_ROW_COUNT;
					restore_depth = -1;
					restore_relative = -1;
					restore_selected = 0;
					restore_start = 0;
					restore_end = 0;
				}
				Entry_open(selected);
				if (top->entries->count > 0) readyEntryResume(top->entries->items[top->selected]);
				dirty = 1;
			}
			else if (PAD_justPressed(BTN_RIGHT)) {
				if(qm_row == 0 && qm_total > qm_slots) {
					qm_col++;
					if(qm_col >= qm_total) {
						qm_col = 0;
						qm_shift = 0;
						qm_slot = 0;
					}
					else {
						qm_slot++;
						if(qm_slot >= qm_slots) {
							qm_slot = qm_slots - 1;
							qm_shift++;
						}
					}
				}
				else {
					qm_col += 1;
					if(qm_col >= qm_total) {
						qm_col = 0;
					}
				}
				dirty = 1;
			}
			else if (PAD_justPressed(BTN_LEFT)) {
				if(qm_row == 0  && qm_total > qm_slots) {
					qm_col -= 1;
					if(qm_col < 0) {
						qm_col = qm_total - 1;
						qm_shift = qm_total - qm_slots;
						qm_slot = qm_slots - 1;
					}
					else {
						qm_slot--;
						if(qm_slot < 0) {
							qm_slot = 0;
							qm_shift--;
						}
					}
				}
				else {
					qm_col -= 1;
					if(qm_col < 0) {
						qm_col = qm_total - 1;
					}
				}
				dirty = 1;
			}
			else if(PAD_justPressed(BTN_DOWN)) {
				if(qm_row == 0) {
					qm_row = 1;
					qm_col = 0;
					dirty = 1;
				}
			}
			else if(PAD_justPressed(BTN_UP)) {
				if(qm_row == 1) {
					qm_row = 0;
					qm_col = qm_slot + qm_shift;
					dirty = 1;
				}
			}
		}
		else if(currentScreen == SCREEN_GAMESWITCHER) {
			if (PAD_justPressed(BTN_B) || PAD_tappedSelect(now)) {
				currentScreen = SCREEN_GAMELIST;
				switcher_selected = 0;
				dirty = 1;
				folderbgchanged = 1; // The background painting code is a clusterfuck, just force a repaint here
			}
			else if (recents->count > 0 && PAD_justReleased(BTN_A)) {
				// this will drop us back into game switcher after leaving the game
				putFile(GAME_SWITCHER_PERSIST_PATH, "unused");
				startgame = 1;
				Entry *selectedEntry = entryFromRecent(recents->items[switcher_selected]);
				readyEntryResume(selectedEntry);
				should_resume_pak = (selectedEntry->type==ENTRY_PAK && pak_can_resume);
				Entry_open(selectedEntry);
				dirty = 1;
				Entry_free(selectedEntry);
			}
			else if (recents->count > 0 && PAD_justReleased(BTN_Y)) {
				// remove
				Recent* recentEntry = recents->items[switcher_selected--];
				Array_remove(recents, recentEntry);
				Recent_free(recentEntry);
				saveRecents();
				if(switcher_selected < 0)
					switcher_selected = recents->count - 1; // wrap
				dirty = 1;
			}
			else if (PAD_justPressed(BTN_RIGHT)) {
				switcher_selected++;
				if(switcher_selected == recents->count)
					switcher_selected = 0; // wrap
				dirty = 1;
				gsanimdir = SLIDE_LEFT;
			}
			else if (PAD_justPressed(BTN_LEFT)) {
				switcher_selected--;
				if(switcher_selected < 0)
					switcher_selected = recents->count - 1; // wrap
				dirty = 1;
				gsanimdir = SLIDE_RIGHT;
			}
		}
		else {
			if (PAD_tappedMenu(now)) {
				currentScreen = SCREEN_QUICKMENU;
				qm_col = 0;
				qm_row = 0;
				qm_shift = 0;
				qm_slot = 0;
				dirty = 1;
				folderbgchanged = 1; // The background painting code is a clusterfuck, just force a repaint here
				if (!HAS_POWER_BUTTON && !simple_mode) PWR_enableSleep();
			}
			else if (PAD_tappedSelect(now)) {
				currentScreen = SCREEN_GAMESWITCHER;
				switcher_selected = 0;
				dirty = 1;
			}
			else if (total>0) {
				if (PAD_justRepeated(BTN_UP)) {
					if (selected==0 && !PAD_justPressed(BTN_UP)) {
						// stop at top
					}
					else {
						selected -= 1;
						if (selected<0) {
							selected = total-1;
							int start = total - MAIN_ROW_COUNT;
							top->start = (start<0) ? 0 : start;
							top->end = total;
						}
						else if (selected<top->start) {
							top->start -= 1;
							top->end -= 1;
						}
					}
				}
				else if (PAD_justRepeated(BTN_DOWN)) {
					if (selected==total-1 && !PAD_justPressed(BTN_DOWN)) {
						// stop at bottom
					}
					else {
						selected += 1;
						if (selected>=total) {
							selected = 0;
							top->start = 0;
							top->end = (total<MAIN_ROW_COUNT) ? total : MAIN_ROW_COUNT;
						}
						else if (selected>=top->end) {
							top->start += 1;
							top->end += 1;
						}
					}
				}
				if (PAD_justRepeated(BTN_LEFT)) {
					selected -= MAIN_ROW_COUNT;
					if (selected<0) {
						selected = 0;
						top->start = 0;
						top->end = (total<MAIN_ROW_COUNT) ? total : MAIN_ROW_COUNT;
					}
					else if (selected<top->start) {
						top->start -= MAIN_ROW_COUNT;
						if (top->start<0) top->start = 0;
						top->end = top->start + MAIN_ROW_COUNT;
					}
				}
				else if (PAD_justRepeated(BTN_RIGHT)) {
					selected += MAIN_ROW_COUNT;
					if (selected>=total) {
						selected = total-1;
						int start = total - MAIN_ROW_COUNT;
						top->start = (start<0) ? 0 : start;
						top->end = total;
					}
					else if (selected>=top->end) {
						top->end += MAIN_ROW_COUNT;
						if (top->end>total) top->end = total;
						top->start = top->end - MAIN_ROW_COUNT;
					}
				}
			}

			if (PAD_justRepeated(BTN_L1) && !PAD_isPressed(BTN_R1) && !PWR_ignoreSettingInput(BTN_L1, show_setting)) { // previous alpha
				Entry* entry = top->entries->items[selected];
				int i = entry->alpha-1;
				if (i>=0) {
					selected = top->alphas->items[i];
					if (total>MAIN_ROW_COUNT) {
						top->start = selected;
						top->end = top->start + MAIN_ROW_COUNT;
						if (top->end>total) top->end = total;
						top->start = top->end - MAIN_ROW_COUNT;
					}
				}
			}
			else if (PAD_justRepeated(BTN_R1) && !PAD_isPressed(BTN_L1) && !PWR_ignoreSettingInput(BTN_R1, show_setting)) { // next alpha
				Entry* entry = top->entries->items[selected];
				int i = entry->alpha+1;
				if (i<top->alphas->count) {
					selected = top->alphas->items[i];
					if (total>MAIN_ROW_COUNT) {
						top->start = selected;
						top->end = top->start + MAIN_ROW_COUNT;
						if (top->end>total) top->end = total;
						top->start = top->end - MAIN_ROW_COUNT;
					}
				}
			}

			if (selected!=top->selected) {
				top->selected = selected;
				dirty = 1;
			}

			Entry* entry = top->entries->items[top->selected];

			if (dirty && total>0)
				readyEntryResume(entry);

			if (total>0
			    && entry->type==ENTRY_PAK && pak_can_resume
			    && PAD_justReleased(BTN_RESUME)) {
				should_resume_pak = 1;
				Entry_open(entry);

				dirty = 1;
			}
			else if (total>0 && PAD_justPressed(BTN_A)) {
				Entry_open(entry);
				if(entry->type == ENTRY_DIR) {
					animationdirection = SLIDE_LEFT;
					total = top->entries->count;
				}
				dirty = 1;

				if (total>0) readyEntryResume(top->entries->items[top->selected]);
			}
			else if (PAD_justPressed(BTN_B) && stack->count>1) {
				closeDirectory();
				animationdirection = SLIDE_RIGHT;
				total = top->entries->count;
				dirty = 1;

				if (total>0) readyEntryResume(top->entries->items[top->selected]);
			}
		}
		}

		if (home_overlay_dirty) {
			dirty = 1;
			home_overlay_dirty = 0;
		}

		if(dirty) {
			SDL_Surface *tmpOldScreen = NULL;
			SDL_Surface * switcherSur = NULL;
			// NOTE:22 This causes slowdown when CFG_getMenuTransitions is set to false because animationdirection turns > 0 somewhere but is never set back to 0 and so this code runs on every action, will fix later
			if(animationdirection != ANIM_NONE || (lastScreen==SCREEN_GAMELIST && currentScreen == SCREEN_GAMESWITCHER)) {
				if(tmpOldScreen) SDL_FreeSurface(tmpOldScreen);
				tmpOldScreen = GFX_captureRendererToSurface();
				SDL_SetSurfaceBlendMode(tmpOldScreen,SDL_BLENDMODE_BLEND);
			}

			// clear only background layer on start
			if(lastScreen==SCREEN_GAME || lastScreen==SCREEN_OFF) {
				GFX_clearLayers(LAYER_ALL);
			}
			else {
				GFX_clearLayers(LAYER_TRANSITION);
				if(lastScreen!=SCREEN_GAMELIST)
					GFX_clearLayers(LAYER_THUMBNAIL);
				GFX_clearLayers(LAYER_SCROLLTEXT);
				GFX_clearLayers(LAYER_IDK2);
			}
			GFX_clear(screen);

			if(CFG_getGameSwitcherCurtain() > 0 && currentScreen == SCREEN_GAMESWITCHER) {
				GFX_blitTopCurtain(screen);
				GFX_blitBottomCurtain(screen);
			}

			int ow = GFX_blitHardwareGroup(screen, show_setting);
			if (currentScreen == SCREEN_QUICKMENU) {
				if(lastScreen != SCREEN_QUICKMENU) {
					GFX_clearLayers(LAYER_BACKGROUND);
					GFX_clearLayers(LAYER_THUMBNAIL);
				}

				Entry *current = qm_row == 0 ? quick->items[qm_col] : quickActions->items[qm_col];
				char newBgPath[MAX_PATH];
				char fallbackBgPath[MAX_PATH];

				sprintf(newBgPath, SDCARD_PATH "/.media/quick_%s%s.png", current->name, 
					!strcmp(current->name,"Wifi") && !CFG_getWifi() || 							// wifi or wifi_off, based on state
					!strcmp(current->name,"Bluetooth") && !CFG_getBluetooth() ? "_off" : "");	// bluetooth or bluetooth_off, based on state
				sprintf(fallbackBgPath, SDCARD_PATH "/.media/quick.png");

				// background
				if(!exists(newBgPath))
					strncpy(newBgPath, fallbackBgPath, sizeof(newBgPath) - 1);

				if(strcmp(newBgPath, folderBgPath) != 0) {
					strncpy(folderBgPath, newBgPath, sizeof(folderBgPath) - 1);
					startLoadFolderBackground(newBgPath, onBackgroundLoaded, NULL);
				}

				// buttons (duped and trimmed from below)
				if (show_setting && !GetHDMI()) GFX_blitHardwareHints(screen, show_setting);
				else GFX_blitButtonGroup((char*[]){ BTN_SLEEP==BTN_POWER?"POWER":"MENU",(char*)TR("SLEEP"),  NULL }, 0, screen, 0);

				GFX_blitButtonGroup((char*[]){ "B",(char*)TR("BACK"), "A",(char*)TR("OPEN"), NULL }, 1, screen, 1);

				if(CFG_getShowQuickswitcherUI()) {
					#define MENU_ITEM_SIZE 72 // item size, top line
					#define MENU_MARGIN_Y 32 // space between main UI elements and quick menu
					#define MENU_MARGIN_X 40 // space between main UI elements and quick menu
					#define MENU_ITEM_MARGIN 18 // space between items, top line
					#define MENU_TOGGLE_MARGIN 8 // space between items, bottom line
					#define MENU_LINE_MARGIN 8 // space between top and bottom line

					// this is flexible, not sure I like it at smaller scales than 3 though
					//int item_size = screen->h - SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN + // top pill area
					//	MENU_MARGIN_Y + MENU_LINE_MARGIN + PILL_SIZE + MENU_MARGIN_Y + // our own area
					//	BUTTON_MARGIN + PILL_SIZE + PADDING); // bottom pill area

					int item_space_y = screen->h - SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN + // top pill area
						MENU_MARGIN_Y + MENU_LINE_MARGIN + PILL_SIZE + MENU_MARGIN_Y + // our own area
						BUTTON_MARGIN + PILL_SIZE + PADDING);
					int item_size = SCALE1(MENU_ITEM_SIZE);
					int item_extra_y = item_space_y - item_size;
					int item_space_x = screen->w - SCALE1(PADDING + MENU_MARGIN_X + MENU_MARGIN_X + PADDING);
					// extra left margin for the first item in order to properly center all of them in the
					// available space
					int item_inset_x = (item_space_x - SCALE1(qm_slots * MENU_ITEM_SIZE + (qm_slots - 1) * MENU_ITEM_MARGIN)) / 2;

					// primary
					ox = SCALE1(PADDING + MENU_MARGIN_X) + item_inset_x;
					oy = SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN + MENU_MARGIN_Y) + item_extra_y / 2;
					// just to keep selection visible.
					// every display should be able to fit three items, we shift horizontally to accomodate.
					ox -= qm_shift * (item_size + SCALE1(MENU_ITEM_MARGIN));
					for (int c = 0; c < quick->count; c++)
					{
						SDL_Rect item_rect = {ox, oy, item_size, item_size};
						Entry *item = quick->items[c];

						SDL_Color text_color = uintToColour(THEME_COLOR4_255);
						uint32_t item_color = THEME_COLOR3;
						uint32_t icon_color = THEME_COLOR4;

						if(qm_row == 0 && qm_col == c) {
							text_color = uintToColour(THEME_COLOR5_255);
							item_color = THEME_COLOR1;
							icon_color = THEME_COLOR5;
						}

						GFX_blitRectColor(ASSET_STATE_BG, screen, &item_rect, item_color);

						char icon_path[MAX_PATH];
						sprintf(icon_path, SDCARD_PATH "/.system/res/%s@%ix.png", item->name, FIXED_SCALE);
						SDL_Surface* bmp = IMG_Load(icon_path);
						if(bmp) {
							SDL_Surface* converted = SDL_ConvertSurfaceFormat(bmp, screen->format->format, 0);
							if (converted) {
								SDL_FreeSurface(bmp);
								bmp = converted;
							}
						}
						if(bmp) {
							// Calculate the position to center the source surface
							int x = (item_rect.w - bmp->w) / 2;
							int y = (item_rect.h - SCALE1(FONT_TINY + BUTTON_MARGIN) - bmp->h) / 2;
							SDL_Rect destRect = { ox+x, oy+y, 0, 0 };  // width/height not required
							//SDL_BlitSurface(bmp, NULL, screen, &destRect);

							GFX_blitSurfaceColor(bmp, NULL, screen, &destRect, icon_color);
						}

						int w, h;
						GFX_sizeText(font.tiny, item->name, SCALE1(FONT_TINY), &w, &h);
						SDL_Rect text_rect = {item_rect.x + (item_size - w) / 2, item_rect.y + item_size - h - SCALE1(BUTTON_MARGIN), w, h};
						GFX_blitText(font.tiny, item->name, SCALE1(FONT_TINY), text_color, screen, &text_rect);

						ox += item_rect.w + SCALE1(MENU_ITEM_MARGIN);
					}

					// secondary
					ox = SCALE1(PADDING + MENU_MARGIN_X);
					ox += (screen->w - SCALE1(PADDING + MENU_MARGIN_X + MENU_MARGIN_X + PADDING) - SCALE1(quickActions->count * PILL_SIZE) - SCALE1((quickActions->count - 1) * MENU_TOGGLE_MARGIN))/2;
					oy = SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN + MENU_MARGIN_Y + MENU_LINE_MARGIN) + item_size + item_extra_y / 2;
					for (int c = 0; c < quickActions->count; c++) {
						SDL_Rect item_rect = {ox, oy, SCALE1(PILL_SIZE), SCALE1(PILL_SIZE)};
						Entry *item = quickActions->items[c];

						SDL_Color text_color = uintToColour(THEME_COLOR4_255);
						uint32_t item_color = THEME_COLOR3;
						uint32_t icon_color = THEME_COLOR4;

						if(qm_row == 1 && qm_col == c) {
							text_color = uintToColour(THEME_COLOR5_255);
							item_color = THEME_COLOR1;
							icon_color = THEME_COLOR5;
						}

						GFX_blitPillColor(ASSET_WHITE_PILL, screen, &item_rect, item_color, RGB_WHITE);

						int asset = ASSET_WIFI_OFF;
						if (!strcmp(item->name,"Wifi"))
							asset = CFG_getWifi() ? ASSET_WIFI : ASSET_WIFI_OFF;
						else if (!strcmp(item->name,"Bluetooth"))
							asset = CFG_getBluetooth() ? ASSET_BLUETOOTH : ASSET_BLUETOOTH_OFF;
						else if (!strcmp(item->name,"Sleep"))
							asset = ASSET_SUSPEND;
						else if (!strcmp(item->name,"Reboot"))
							asset = ASSET_RESTART;
						else if (!strcmp(item->name,"Poweroff"))
							asset = ASSET_POWEROFF;
						else if (!strcmp(item->name,"Settings"))
							asset = ASSET_SETTINGS;

						SDL_Rect rect;
						GFX_assetRect(asset, &rect);
						int x = item_rect.x;
						int y = item_rect.y;
						x += (SCALE1(PILL_SIZE) - rect.w) / 2;
						y += (SCALE1(PILL_SIZE) - rect.h) / 2;

						GFX_blitAssetColor(asset, NULL, screen, &(SDL_Rect){x,y}, icon_color);

						ox += item_rect.w + SCALE1(MENU_TOGGLE_MARGIN);
					}
				}
				lastScreen = SCREEN_QUICKMENU;
			}
			else if(startgame) {
				//pilltargetY = +screen->w;
				//animationdirection = ANIM_NONE;
				GFX_clearLayers(LAYER_ALL);
				GFX_clear(screen);
				GFX_flipHidden();
				GFX_animateSurfaceOpacity(tmpOldScreen,0,0,screen->w,screen->h,255,0,CFG_getMenuTransitions() ? 150:20,LAYER_BACKGROUND);
			}
			else if(currentScreen == SCREEN_GAMESWITCHER) {
				GFX_clearLayers(LAYER_ALL);
				ox = 0;
				oy = 0;

				// For all recents with resumable state (i.e. has savegame), show game switcher carousel
				if(recents->count > 0) {
					Entry *selectedEntry = entryFromRecent(recents->items[switcher_selected]);
					readyEntryResume(selectedEntry);
					// title pill
					{
						int max_width = screen->w - SCALE1(PADDING * 2) - ow;

						char display_name[256];
						int text_width = GFX_truncateText(font.large, selectedEntry->name, display_name, max_width, SCALE1(BUTTON_PADDING*2));
						max_width = MIN(max_width, text_width);

						SDL_Surface* text;
						SDL_Color textColor = uintToColour(THEME_COLOR6_255);
						SDL_LockMutex(fontMutex);
						text = TTF_RenderUTF8_Blended(font.large, display_name, textColor);
						SDL_UnlockMutex(fontMutex);
						const int text_offset_y = (SCALE1(PILL_SIZE) - text->h + 1) >> 1;
						GFX_blitPillLight(ASSET_WHITE_PILL, screen, &(SDL_Rect){
							SCALE1(PADDING),
							SCALE1(PADDING),
							max_width,
							SCALE1(PILL_SIZE)
						});
						SDL_BlitSurface(text, &(SDL_Rect){
							0,
							0,
							max_width-SCALE1(BUTTON_PADDING*2),
							text->h
						}, screen, &(SDL_Rect){
							SCALE1(PADDING+BUTTON_PADDING),
							SCALE1(PADDING) + text_offset_y,
						});
						SDL_FreeSurface(text);
					}

					GFX_blitButtonGroup((char*[]){ "B",(char*)TR("BACK"),  NULL }, 0, screen, 0);

					if(pak_can_resume) GFX_blitButtonGroup((char*[]){ "Y", (char*)TR("REMOVE"), "A",(char*)TR("RESUME"), NULL }, 1, screen, 1);
					else GFX_blitButtonGroup((char*[]){ "A",(char*)TR("OPEN"), NULL }, 1, screen, 1);
					Entry_free(selectedEntry);
				}
				else {
					SDL_Rect preview_rect = {ox,oy,screen->w,screen->h};
					SDL_FillRect(screen, &preview_rect, 0);
					GFX_blitMessage(font.large, (char*)TR("No Recents"), screen, &preview_rect);
					GFX_blitButtonGroup((char*[]){ "B",(char*)TR("BACK"), NULL }, 1, screen, 1);
				}

				GFX_flipHidden();

				if(switcherSur) SDL_FreeSurface(switcherSur);
				switcherSur = GFX_captureRendererToSurface();
				lastScreen = SCREEN_GAMESWITCHER;
			}
			else { // if currentscreen == SCREEN_GAMELIST
				// background and game art file path stuff
				Entry* entry = top->entries->items[top->selected];
				assert(entry);
				char tmp_path[MAX_PATH];
				strncpy(tmp_path, entry->path, sizeof(tmp_path) - 1);
				tmp_path[sizeof(tmp_path) - 1] = '\0';

				char* res_name = strrchr(tmp_path, '/');
				if (res_name) res_name++;

				char path_copy[1024];
				strncpy(path_copy, entry->path, sizeof(path_copy) - 1);
				path_copy[sizeof(path_copy) - 1] = '\0';

				char* rompath = dirname(path_copy);

				char res_copy[1024];
				strncpy(res_copy, res_name, sizeof(res_copy) - 1);
				res_copy[sizeof(res_copy) - 1] = '\0';

				char* dot = strrchr(res_copy, '.');
				if (dot) *dot = '\0';

				static int lastType = -1;

				// this is only a choice on the root folder
				list_show_entry_names = stack->count > 1 || CFG_getShowFolderNamesAtRoot();

				// load folder background
				char defaultBgPath[512];
				snprintf(defaultBgPath, sizeof(defaultBgPath), SDCARD_PATH "/bg.png");

				if(entry->type == ENTRY_DIR && CFG_getRomsUseFolderBackground()) {
					char *newBg = entry->path;
					if((strcmp(newBg, folderBgPath) != 0 || lastType != entry->type) && sizeof(folderBgPath) != 1) {
						lastType = entry->type;
						char tmppath[512];
						strncpy(folderBgPath, newBg, sizeof(folderBgPath) - 1);
						snprintf(tmppath, sizeof(tmppath), "%s/.media/bg.png", folderBgPath);
						if(!exists(tmppath)) {
							// Safeguard: If no background is available, still render the text to leave the user a way out
							list_show_entry_names = true;
							snprintf(tmppath, sizeof(tmppath), defaultBgPath, folderBgPath);
						}
						startLoadFolderBackground(tmppath, onBackgroundLoaded, NULL);
					}
				}
				else if(strcmp(defaultBgPath, folderBgPath) != 0 && exists(defaultBgPath)) {
					strncpy(folderBgPath, defaultBgPath, sizeof(folderBgPath) - 1);
					startLoadFolderBackground(defaultBgPath, onBackgroundLoaded, NULL);
				}
				else {
					// Safeguard: If no background is available, still render the text to leave the user a way out
					list_show_entry_names = true;
				}
				// load game thumbnails
				if (total > 0) {
					if(CFG_getShowGameArt()) {
						char thumbpath[1024];
						snprintf(thumbpath, sizeof(thumbpath), "%s/.media/%s.png", rompath, res_copy);
						had_thumb = 0;
						startLoadThumb(thumbpath, onThumbLoaded, NULL);
						int max_w = (int)(screen->w - (screen->w * CFG_getGameArtWidth()));
						int max_h = (int)(screen->h * 0.6);
						int new_w = max_w;
						int new_h = max_h;
						if(exists(thumbpath)) {
							ox = (int)(max_w) - SCALE1(BUTTON_MARGIN*5);
							had_thumb = 1;
						}
						else
							ox = screen->w;
					}
				}

				// buttons
				if (show_setting && !GetHDMI()) GFX_blitHardwareHints(screen, show_setting);
				else if (pak_can_resume) GFX_blitButtonGroup((char*[]){ "X",(char*)TR("RESUME"),  NULL }, 0, screen, 0);
				else GFX_blitButtonGroup((char*[]){
					BTN_SLEEP==BTN_POWER?"POWER":"MENU",
					BTN_SLEEP==BTN_POWER||simple_mode?(char*)TR("SLEEP"):(char*)TR("INFO"),
					NULL }, 0, screen, 0);

				if (total==0) {
					if (stack->count>1) {
						GFX_blitButtonGroup((char*[]){ "B",(char*)TR("BACK"),  NULL }, 0, screen, 1);
					}
				}
				else {
					if (stack->count>1) {
						GFX_blitButtonGroup((char*[]){ "B",(char*)TR("BACK"), "A",(char*)TR("OPEN"), NULL }, 1, screen, 1);
					}
					else {
						GFX_blitButtonGroup((char*[]){ "A",(char*)TR("OPEN"), NULL }, 0, screen, 1);
					}
				}

				// list
				if (total > 0) {
					selected_row = top->selected - top->start;
					previousY = previous_row * PILL_SIZE;
					targetY = selected_row * PILL_SIZE;
					SDL_Color text_color = uintToColour(THEME_COLOR4_255); // list text color
					for (int i = top->start, j = 0; i < top->end; i++, j++) {
						Entry* entry = top->entries->items[i];
						char* entry_name = entry->name;
						char* entry_unique = entry->unique;
						int available_width = MAX(0,(had_thumb ? ox + SCALE1(BUTTON_MARGIN) : screen->w - SCALE1(BUTTON_MARGIN)) - SCALE1(PADDING * 2));
						bool row_is_selected = (j == selected_row);
						bool row_is_top = (i == top->start);
						bool row_has_moved = (previous_row != selected_row || previous_depth != stack->count);
						if (row_is_top && !(had_thumb))
							available_width -= ow;

						trimSortingMeta(&entry_name);
						if (entry_unique) // Only render if a unique name exists
							trimSortingMeta(&entry_unique);

						char display_name[256];
						int text_width = GFX_getTextWidth(font.large, entry_unique ? entry_unique : entry_name, display_name, available_width, SCALE1(BUTTON_PADDING * 2));
						int max_width = MIN(available_width, text_width);

						// This spaghetti is preventing white text on white pill when volume/color temp is shown,
						// dont ask me why. This all needs to get tossed out and redone properly later.
						SDL_Color text_color = uintToColour(THEME_COLOR4_255);
						int notext = 0;
						if(!row_has_moved && row_is_selected) {
							text_color = uintToColour(THEME_COLOR5_255);
							notext = 1;
						}

						SDL_LockMutex(fontMutex);
						SDL_Surface* text = TTF_RenderUTF8_Blended(font.large, entry_name, text_color);
						SDL_Surface* text_unique = TTF_RenderUTF8_Blended(font.large, display_name, COLOR_DARK_TEXT);
						SDL_UnlockMutex(fontMutex);
						// TODO: Use actual font metrics to center, this only works in simple cases
						const int text_offset_y = (SCALE1(PILL_SIZE) - text->h + 1) >> 1;
						if (row_is_selected) {
							is_scrolling = list_show_entry_names && GFX_textShouldScroll(font.large,display_name, max_width - SCALE1(BUTTON_PADDING*2), fontMutex);
							GFX_resetScrollText();
							bool should_animate = previous_depth == stack->count;
							SDL_LockMutex(animMutex);
							if(globalpill) {
								SDL_FreeSurface(globalpill);
								globalpill=NULL;
							}
							globalpill = SDL_CreateRGBSurfaceWithFormat(SDL_SWSURFACE, max_width, SCALE1(PILL_SIZE), FIXED_DEPTH, screen->format->format);
							GFX_blitPillDark(ASSET_WHITE_PILL, globalpill, &(SDL_Rect){0,0, max_width, SCALE1(PILL_SIZE)});
							globallpillW =  max_width;
							SDL_UnlockMutex(animMutex);
							updatePillTextSurface(entry_name, max_width, uintToColour(THEME_COLOR5_255));
							AnimTask* task = malloc(sizeof(AnimTask));
							task->startX = SCALE1(BUTTON_MARGIN);
							task->startY = SCALE1(previousY+PADDING);
							task->targetX = SCALE1(BUTTON_MARGIN);
							task->targetY = SCALE1(targetY+PADDING);
							task->targetTextY = SCALE1(PADDING + targetY) + text_offset_y;
							pilltargetTextY = task->targetTextY;
							task->move_w = max_width;
							task->move_h = SCALE1(PILL_SIZE);
							task->frames = should_animate && CFG_getMenuAnimations() ? 3:1;
							task->entry_name = strdup(notext ? " " : entry_name);
							animPill(task);
						}
						SDL_Rect text_rect = { 0, 0, max_width - SCALE1(BUTTON_PADDING*2), text->h };
						SDL_Rect dest_rect = { SCALE1(BUTTON_MARGIN + BUTTON_PADDING), SCALE1(PADDING + (j * PILL_SIZE)) + text_offset_y };

						if(list_show_entry_names) {
							SDL_BlitSurface(text_unique, &text_rect, screen, &dest_rect);
							SDL_BlitSurface(text, &text_rect, screen, &dest_rect);
						}
						SDL_FreeSurface(text_unique); // Free after use
						SDL_FreeSurface(text); // Free after use
					}
					if(lastScreen==SCREEN_GAMESWITCHER) {
						if(switcherSur) {
							// update cpu surface here first
							GFX_clearLayers(LAYER_ALL);
							folderbgchanged=1;

							GFX_flipHidden();
							GFX_animateSurface(switcherSur,0,0,0,0-screen->h,screen->w,screen->h,CFG_getMenuTransitions() ? 100:20,255,255,LAYER_BACKGROUND);
							animationdirection = ANIM_NONE;
						}
					}
					if(lastScreen==SCREEN_OFF) {
						GFX_animateSurfaceOpacity(blackBG,0,0,screen->w,screen->h,255,0,CFG_getMenuTransitions() ? 200:20,LAYER_THUMBNAIL);
					}

					previous_row = selected_row;
					previous_depth = stack->count;
				}
				else {
					// TODO: for some reason screen's dimensions end up being 0x0 in GFX_blitMessage...
					GFX_blitMessage(font.large, (char*)TR("Empty folder"), screen, &(SDL_Rect){0,0,screen->w,screen->h}); //, NULL);
				}

				lastScreen = SCREEN_GAMELIST;
			}

			if(animationdirection != ANIM_NONE) {
				if(CFG_getMenuTransitions()) {
					SDL_LockMutex(bgMutex);
					if(folderbgbmp) {
						GFX_drawOnLayer(folderbgbmp, 0, 0, screen->w, screen->h, 1.0f, 0, LAYER_BACKGROUND);
					} else {
						GFX_clearLayers(LAYER_BACKGROUND);
					}
					SDL_UnlockMutex(bgMutex);
					folderbgchanged = 1;
					GFX_clearLayers(LAYER_TRANSITION);
					// Draw the selection pill at its target position before capturing the new
					// screen, so the cursor is present in the incoming slide surface rather than
					// blinking in after the transition animation ends.
					if (currentScreen == SCREEN_GAMELIST && total > 0 && globalpill && list_show_entry_names) {
						SDL_LockMutex(animMutex);
						GFX_drawOnLayer(globalpill, SCALE1(BUTTON_MARGIN), (int)SCALE1(targetY + PADDING), globallpillW, globalpill->h, 1.0f, 0, LAYER_TRANSITION);
						if (globalText) {
							GFX_drawOnLayer(globalText, SCALE1(BUTTON_MARGIN + BUTTON_PADDING), pilltargetTextY, globalText->w, globalText->h, 1.0f, 0, LAYER_SCROLLTEXT);
						}
						SDL_UnlockMutex(animMutex);
					}
					GFX_flipHidden();
					SDL_Surface *tmpNewScreen = GFX_captureRendererToSurface();
					SDL_SetSurfaceBlendMode(tmpNewScreen,SDL_BLENDMODE_BLEND);
					GFX_clearLayers(LAYER_SCROLLTEXT); // baked into tmpNewScreen; must clear or it floats above the animation
					GFX_clearLayers(LAYER_THUMBNAIL);
					{
						int _duration = (CFG_getMenuTransitions() == TRANSITION_COMFY)
							? TRANSITION_COMFY_DURATION
							: TRANSITION_SNAPPY_DURATION;
						if(animationdirection == SLIDE_LEFT) GFX_animateAndFadeSurface(tmpOldScreen,0,0,-250,0,FIXED_WIDTH,FIXED_HEIGHT,_duration,tmpNewScreen,FIXED_WIDTH,0,0,0,FIXED_WIDTH,FIXED_HEIGHT,255,255,LAYER_THUMBNAIL,TRANSITION_CURVE,TRANSITION_CURVE,TRANSITION_INTENSITY);
						if(animationdirection == SLIDE_RIGHT) GFX_animateAndFadeSurface(tmpNewScreen,-250,0,0,0,FIXED_WIDTH,FIXED_HEIGHT,_duration,tmpOldScreen,0,0,FIXED_WIDTH,0,FIXED_WIDTH,FIXED_HEIGHT,255,255,LAYER_THUMBNAIL,TRANSITION_CURVE,TRANSITION_CURVE,TRANSITION_INTENSITY);
					}
					GFX_clearLayers(LAYER_THUMBNAIL);
					SDL_FreeSurface(tmpNewScreen);
				}
				// animation done
				animationdirection = ANIM_NONE;
			}

			if(lastScreen == SCREEN_QUICKMENU) {
				SDL_LockMutex(bgMutex);
				if(folderbgchanged) {
					if(folderbgbmp)
						GFX_drawOnLayer(folderbgbmp,0, 0, screen->w, screen->h,1.0f,0,LAYER_BACKGROUND);
					else {
						GFX_clearLayers(LAYER_BACKGROUND);
					}
					folderbgchanged = 0;
				}
				SDL_UnlockMutex(bgMutex);
			}
			else if(lastScreen == SCREEN_GAMELIST) {
				SDL_LockMutex(bgMutex);
				if(folderbgchanged) {
					if(folderbgbmp)
						GFX_drawOnLayer(folderbgbmp,0, 0, screen->w, screen->h,1.0f,0,LAYER_BACKGROUND);
					else {
						GFX_clearLayers(LAYER_BACKGROUND);
					}
					folderbgchanged = 0;
				}
				SDL_UnlockMutex(bgMutex);
				SDL_LockMutex(thumbMutex);
				if(thumbbmp && thumbchanged) {
					int img_w = thumbbmp->w;
					int img_h = thumbbmp->h;
					double aspect_ratio = (double)img_h / img_w;
					int max_w = (int)(screen->w * CFG_getGameArtWidth());
					int max_h = (int)(screen->h * 0.6);
					int new_w = max_w;
					int new_h = (int)(new_w * aspect_ratio);

					if (new_h > max_h) {
						new_h = max_h;
						new_w = (int)(new_h / aspect_ratio);
					}

					int target_x = screen->w-(new_w + SCALE1(BUTTON_MARGIN*3));
					int target_y = (int)(screen->h * 0.50);
					int center_y = target_y - (new_h / 2); // FIX: use new_h instead of thumbbmp->h
					GFX_clearLayers(LAYER_THUMBNAIL);
					GFX_drawOnLayer(thumbbmp,target_x,center_y,new_w,new_h,1.0f,0,LAYER_THUMBNAIL);
				} else if(thumbchanged) {
					GFX_clearLayers(LAYER_THUMBNAIL);
				}
				SDL_UnlockMutex(thumbMutex);

				GFX_clearLayers(LAYER_TRANSITION);
				GFX_clearLayers(LAYER_SCROLLTEXT);

				SDL_LockMutex(animMutex);
				if (list_show_entry_names) {
					GFX_drawOnLayer(globalpill, pillRect.x, pillRect.y, globallpillW, globalpill->h, 1.0f, 0, LAYER_TRANSITION);
					GFX_drawOnLayer(globalText, SCALE1(BUTTON_MARGIN + BUTTON_PADDING), pilltargetTextY, globalText->w, globalText->h, 1.0f, 0, LAYER_SCROLLTEXT);
				}
				SDL_UnlockMutex(animMutex);
			}
			if (home_overlay && UIOverlay_isOpen(home_overlay))
				UIOverlay_draw(home_overlay, screen);
			if(!startgame) // dont flip if game gonna start
				GFX_flip(screen);

			dirty = 0;
		} else if(getAnimationDraw() || folderbgchanged || thumbchanged || is_scrolling) {
			// honestly this whole thing is here only for the scrolling text, I set it now to run this at 30fps which is enough for scrolling text, should move this to seperate animation function eventually
			Uint32 now = SDL_GetTicks();
			Uint32 frame_start = now;
			static char cached_display_name[256] = "";
			SDL_LockMutex(bgMutex);
			if(folderbgchanged) {
				if(folderbgbmp)
					GFX_drawOnLayer(folderbgbmp,0, 0, screen->w, screen->h,1.0f,0,LAYER_BACKGROUND);
				else {
					GFX_clearLayers(LAYER_BACKGROUND);
				}
				folderbgchanged = 0;
			}
			SDL_UnlockMutex(bgMutex);
			SDL_LockMutex(thumbMutex);
			if(thumbbmp && thumbchanged) {
				int img_w = thumbbmp->w;
				int img_h = thumbbmp->h;
				double aspect_ratio = (double)img_h / img_w;

				int max_w = (int)(screen->w * CFG_getGameArtWidth());
				int max_h = (int)(screen->h * 0.6);

				int new_w = max_w;
				int new_h = (int)(new_w * aspect_ratio);

				if (new_h > max_h) {
					new_h = max_h;
					new_w = (int)(new_h / aspect_ratio);
				}

				int target_x = screen->w-(new_w + SCALE1(BUTTON_MARGIN*3));
				int target_y = (int)(screen->h * 0.50);
				int center_y = target_y - (new_h / 2); // FIX: use new_h instead of thumbbmp->h
				GFX_clearLayers(LAYER_THUMBNAIL);
				GFX_drawOnLayer(thumbbmp,target_x,center_y,new_w,new_h,1.0f,0,LAYER_THUMBNAIL);
				thumbchanged = 0;
			} else if(thumbchanged) {
				GFX_clearLayers(LAYER_THUMBNAIL);
				thumbchanged = 0;
			}
			SDL_UnlockMutex(thumbMutex);
			SDL_LockMutex(animMutex);
			if (getAnimationDraw()) {
				GFX_clearLayers(LAYER_TRANSITION);
				if (list_show_entry_names)
					GFX_drawOnLayer(globalpill, pillRect.x, pillRect.y, globallpillW, globalpill->h, 1.0f, 0, LAYER_TRANSITION);
				setAnimationDraw(0);
			}
			SDL_UnlockMutex(animMutex);
			if (currentScreen != SCREEN_GAMESWITCHER && currentScreen != SCREEN_QUICKMENU) {
				if(is_scrolling && pillanimdone && currentAnimQueueSize < 1) {
					int ow = GFX_blitHardwareGroup(screen, show_setting);
					Entry* entry = top->entries->items[top->selected];
					trimSortingMeta(&entry->name);
					char* entry_text = entry->name;
					if (entry->unique) {
						trimSortingMeta(&entry->unique);
						entry_text = entry->unique;
					}

					int available_width = (had_thumb ? ox + SCALE1(BUTTON_MARGIN) : screen->w - SCALE1(BUTTON_MARGIN)) - SCALE1(PADDING * 2);
					if (top->selected == top->start && !had_thumb) available_width -= ow;

					SDL_Color text_color = uintToColour(THEME_COLOR5_255);

					int text_width = GFX_getTextWidth(font.large, entry_text, cached_display_name, available_width, SCALE1(BUTTON_PADDING * 2));
					int max_width = MIN(available_width, text_width);
					int text_offset_y = (SCALE1(PILL_SIZE) - TTF_FontHeight(font.large) + 1) >> 1;

					GFX_clearLayers(LAYER_SCROLLTEXT);
					if (list_show_entry_names) {
						GFX_scrollTextTexture(
							font.large,
							entry_text,
							SCALE1(BUTTON_MARGIN + BUTTON_PADDING), SCALE1(PADDING + previous_row * PILL_SIZE) + text_offset_y,
							max_width - SCALE1(BUTTON_PADDING * 2),
							0,
							text_color,
							1,
							fontMutex  // Thread-safe font access
						);
					}
				}
				else {
					GFX_clearLayers(LAYER_TRANSITION);
					GFX_clearLayers(LAYER_SCROLLTEXT);
					SDL_LockMutex(animMutex);
					if (list_show_entry_names) {
						GFX_drawOnLayer(globalpill, pillRect.x, pillRect.y, globallpillW, globalpill->h, 1.0f, 0, LAYER_TRANSITION);
						GFX_drawOnLayer(globalText, SCALE1(BUTTON_MARGIN + BUTTON_PADDING),pilltargetTextY, globalText->w, globalText->h, 1.0f, 0, LAYER_SCROLLTEXT);
					}
					SDL_UnlockMutex(animMutex);
					PLAT_GPU_Flip();
				}
			}
			else {
				GFX_sync();
			}
			dirty = 0;
		}
		else {
			// want to draw only if needed
			SDL_LockMutex(bgqueueMutex);
			SDL_LockMutex(thumbqueueMutex);
			SDL_LockMutex(animqueueMutex);
			if(getNeedDraw()) {
				PLAT_GPU_Flip();
				setNeedDraw(0);
			} else {
				GFX_sync();
			}
			SDL_UnlockMutex(animqueueMutex);
			SDL_UnlockMutex(thumbqueueMutex);
			SDL_UnlockMutex(bgqueueMutex);
		}

		SDL_LockMutex(frameMutex);
		frameReady = true;
		SDL_CondSignal(flipCond);
		SDL_UnlockMutex(frameMutex);

		// animation does not carry over between loops, this should only ever be set by
		// input handling and directly consumed by the following render pass
		assert(animationdirection == ANIM_NONE);

		// handle HDMI change
		static int had_hdmi = -1;
		int has_hdmi = GetHDMI();
		if (had_hdmi==-1) had_hdmi = has_hdmi;
		if (has_hdmi!=had_hdmi) {
			had_hdmi = has_hdmi;

			Entry* entry = top->entries->items[top->selected];
			LOG_info("restarting after HDMI change... (%s)\n", entry->path);
			saveLast(entry->path); // NOTE: doesn't work in Recents (by design)
			sleep(1); // cable-seating debounce; the output switch itself happens at next app start
			quit = 1;
		}
	}

	UIOverlay_destroy(home_overlay);
	home_overlay = NULL;
	Menu_quit();
	PWR_quit();
	PAD_quit();

	// Cleanup worker threads and their synchronization primitives
	cleanupImageLoaderPool();

	GFX_quit(); // Cleanup video subsystem first to stop GPU threads

	// Now safe to free surfaces after GPU threads are stopped
	if(blackBG)	SDL_FreeSurface(blackBG);
	if (folderbgbmp) SDL_FreeSurface(folderbgbmp);
	if (thumbbmp) SDL_FreeSurface(thumbbmp);

	QuitSettings();
}
