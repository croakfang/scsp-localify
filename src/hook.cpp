#include <stdinclude.hpp>
#include <unordered_set>
#include <ranges>
#include <set>
#include <Psapi.h>
#include <intsafe.h>
#include "scgui/scGUIData.hpp"
#include <scgui/scGUIMain.hpp>
#include <mhotkey.hpp>

//using namespace std;

std::function<void()> g_on_hook_ready;
std::function<void()> g_on_close;
std::function<void()> on_hotKey_0;
bool needPrintStack = false;

template<typename T, typename TF>
void convertPtrType(T* cvtTarget, TF func_ptr) {
	*cvtTarget = reinterpret_cast<T>(func_ptr);
}

#pragma region HOOK_MACRO
#define ADD_HOOK(_name_, _fmt_) \
	auto _name_##_offset = reinterpret_cast<void*>(_name_##_addr); \
 	\
	const auto _name_##stat1 = MH_CreateHook(_name_##_offset, _name_##_hook, &_name_##_orig); \
	const auto _name_##stat2 = MH_EnableHook(_name_##_offset); \
	printf(_fmt_##" (%s, %s)\n", _name_##_offset, MH_StatusToString(_name_##stat1), MH_StatusToString(_name_##stat2)); 
#pragma endregion

bool exd = false;
void* SetResolution_orig;
Il2CppString* (*environment_get_stacktrace)();

namespace
{
	void path_game_assembly();
	void patchNP(HMODULE module);
	bool mh_inited = false;
	void* load_library_w_orig = nullptr;
	bool (*CloseNPGameMon)();

	void reopen_self() {
		int result = MessageBoxW(NULL, L"未检测到目标加载，插件初始化失败。是否重启游戏？", L"提示", MB_OKCANCEL);

		if (result == IDOK)
		{
			STARTUPINFOA startupInfo{ .cb = sizeof(STARTUPINFOA) };
			PROCESS_INFORMATION pi{};
			const auto commandLine = GetCommandLineA();
			if (CreateProcessA(NULL, commandLine, NULL, NULL, FALSE, 0, NULL, NULL, &startupInfo, &pi)) {
				// printf("open external plugin: %s (%lu)\n", commandLine, pi.dwProcessId);
				// DWORD dwRetun = 0;
				// WaitForSingleObject(pi.hProcess, INFINITE);
				// GetExitCodeProcess(pi.hProcess, &dwRetun);
				// printf("plugin exit: %d\n", dwRetun);
				CloseHandle(pi.hThread);
				CloseHandle(pi.hProcess);
			}
			TerminateProcess(GetCurrentProcess(), 0);
		}
	}

	HMODULE __stdcall load_library_w_hook(const wchar_t* path)
	{
		using namespace std;
		// printf("load library: %ls\n", path);
		if (!exd && (wstring_view(path).find(L"NPGameDLL.dll") != wstring_view::npos)) {
			// printf("fing NP\n");
			// Sleep(10000000000000);
			// printf("end sleep\n");
			auto ret = reinterpret_cast<decltype(LoadLibraryW)*>(load_library_w_orig)(path);
			patchNP(ret);
			if (environment_get_stacktrace) {
				// printf("NP: %ls\n", environment_get_stacktrace()->start_char);
			}
			return ret;
		}
		// else if (path == L"GameAssembly.dll"sv) {
		//	auto ret = reinterpret_cast<decltype(LoadLibraryW)*>(load_library_w_orig)(path);
		//	return ret;
		// }
		else if (path == L"cri_ware_unity.dll"sv) {
			path_game_assembly();
		}

		return reinterpret_cast<decltype(LoadLibraryW)*>(load_library_w_orig)(path);
	}


	void* (*Encoding_get_UTF8)();
	Il2CppString* (*Encoding_GetString)(void* _this, void* bytes);
	bool isInitedEncoding = false;

	void initEncoding() {
		if (isInitedEncoding) return;
		isInitedEncoding = true;
		convertPtrType(&Encoding_get_UTF8, il2cpp_symbols::get_method_pointer("mscorlib.dll", "System.Text", "Encoding", "get_UTF8", 0));
		convertPtrType(&Encoding_GetString, il2cpp_symbols::get_method_pointer("mscorlib.dll", "System.Text", "Encoding", "GetString", 1) - 0x1B0);
		// convertPtrType(&Encoding_GetString, 0x182ABF800);  // F800 = PTR - 0x1B0
		// printf("Encoding_GetString at %p\n", Encoding_GetString);
		/*
		auto Encoding_GetString_addr = il2cpp_symbols::find_method("mscorlib.dll", "System.Text", "Encoding", [](const MethodInfo* mtd) {
			if (mtd->parameters_count == 1) {
				//if (strcmp(mtd->name, "GetString") == 0) {
				//	printf("GetString Type: %x\n", mtd->parameters->parameter_type->type);
				//}
			}
			return false;
			});
			*/
			
	}

	Il2CppString* bytesToIl2cppString(void* bytes) {
		initEncoding();
		auto encodingInstance = Encoding_get_UTF8();
		// return Encoding_GetString(encodingInstance, bytes);
		return Encoding_GetString(encodingInstance, bytes);
	}

	struct TransparentStringHash : std::hash<std::wstring>, std::hash<std::wstring_view>
	{
		using is_transparent = void;
	};

	typedef std::unordered_set<std::wstring, TransparentStringHash, std::equal_to<void>> AssetPathsType;
	std::map<std::string, AssetPathsType> CustomAssetBundleAssetPaths;
	std::unordered_map<std::string, uint32_t> CustomAssetBundleHandleMap{};
	void* (*AssetBundle_LoadFromFile)(Il2CppString* path, UINT32 crc, UINT64 offset);

	void LoadExtraAssetBundle()
	{
		if (g_extra_assetbundle_paths.empty())
		{
			return;
		}
		// CustomAssetBundleHandleMap.clear();
		// CustomAssetBundleAssetPaths.clear();
		// assert(!ExtraAssetBundleHandle && ExtraAssetBundleAssetPaths.empty());

		static auto AssetBundle_GetAllAssetNames = reinterpret_cast<void* (*)(void*)>(
			il2cpp_resolve_icall("UnityEngine.AssetBundle::GetAllAssetNames()")
			);

		for (const auto& i : g_extra_assetbundle_paths) {
			if (CustomAssetBundleHandleMap.contains(i)) continue;

			const auto extraAssetBundle = AssetBundle_LoadFromFile(il2cpp_string_new(i.c_str()), 0, 0);
			if (extraAssetBundle)
			{
				const auto allAssetPaths = AssetBundle_GetAllAssetNames(extraAssetBundle);
				AssetPathsType assetPath{};
				il2cpp_symbols::iterate_IEnumerable<Il2CppString*>(allAssetPaths, [&assetPath](Il2CppString* path)
					{
						// ExtraAssetBundleAssetPaths.emplace(path->start_char);
						// printf("Asset loaded: %ls\n", path->start_char);
						assetPath.emplace(path->start_char);
					});
				CustomAssetBundleAssetPaths.emplace(i, assetPath);
				CustomAssetBundleHandleMap.emplace(i, il2cpp_gchandle_new(extraAssetBundle, false));
			}
			else
			{
				printf("Cannot load asset bundle: %s\n", i.c_str());
			}
		}
	}

	uint32_t GetBundleHandleByAssetName(std::wstring assetName) {
		for (const auto& i : CustomAssetBundleAssetPaths) {
			for (const auto& m : i.second) {
				if (std::equal(m.begin(), m.end(), assetName.begin(), assetName.end(),
					[](wchar_t c1, wchar_t c2) {
						return std::tolower(c1, std::locale()) == std::tolower(c2, std::locale());
					})) {
					return CustomAssetBundleHandleMap.at(i.first);
				}
			}
		}
		return NULL;
	}

	uint32_t GetBundleHandleByAssetName(std::string assetName) {
		return GetBundleHandleByAssetName(utility::conversions::to_string_t(assetName));
	}

	uint32_t ReplaceFontHandle;
	bool (*Object_IsNativeObjectAlive)(void*);
	void* (*AssetBundle_LoadAsset)(void* _this, Il2CppString* name, Il2CppReflectionType* type);
	Il2CppReflectionType* Font_Type;

	void* getReplaceFont() {
		void* replaceFont{};
		if (g_custom_font_path.empty()) return replaceFont;
		const auto& bundleHandle = GetBundleHandleByAssetName(g_custom_font_path);
		if (bundleHandle)
		{
			if (ReplaceFontHandle)
			{
				replaceFont = il2cpp_gchandle_get_target(ReplaceFontHandle);
				// 加载场景时会被 Resources.UnloadUnusedAssets 干掉，且不受 DontDestroyOnLoad 影响，暂且判断是否存活，并在必要的时候重新加载
				// TODO: 考虑挂载到 GameObject 上
				// AssetBundle 不会被干掉
				if (Object_IsNativeObjectAlive(replaceFont))
				{
					return replaceFont;
				}
				else
				{
					il2cpp_gchandle_free(std::exchange(ReplaceFontHandle, 0));
				}
			}

			const auto extraAssetBundle = il2cpp_gchandle_get_target(bundleHandle);
			replaceFont = AssetBundle_LoadAsset(extraAssetBundle, il2cpp_string_new(g_custom_font_path.c_str()), Font_Type);
			if (replaceFont)
			{
				ReplaceFontHandle = il2cpp_gchandle_new(replaceFont, false);
			}
			else
			{
				std::wprintf(L"Cannot load asset font\n");
			}
		}
		else
		{
			std::wprintf(L"Cannot find asset font\n");
		}
		return replaceFont;
	}

	void* DataFile_GetBytes_orig;
	bool (*DataFile_IsKeyExist)(Il2CppString*);

	void set_fps_hook(int value);
	void set_vsync_count_hook(int value);

	std::filesystem::path dumpBasePath("dumps");

	int dumpScenarioDataById(const std::string& sid) {
		int i = 0;
		int notFoundCount = 0;
		int dumpedCount = 0;
		printf("dumping: %s\n", sid.c_str());
		while (true) {
			const auto dumpName = std::format(L"{}_{:02}.json", utility::conversions::to_utf16string(sid), i);
			const auto dumpNameIl = il2cpp_symbols::NewWStr(dumpName);
			if (!DataFile_IsKeyExist(dumpNameIl)) {
				printf("%ls - continue.\n", dumpName.c_str());
				notFoundCount++;
				if (notFoundCount < 5) {
					i++;
					continue;
				}
				else {
					break;
				}
			}
			auto dataBytes = reinterpret_cast<void* (*)(Il2CppString*)>(DataFile_GetBytes_orig)(dumpNameIl);
			auto newStr = bytesToIl2cppString(dataBytes);
			auto writeWstr = std::wstring(newStr->start_char);
			const std::wstring searchStr = L"\r";
			const std::wstring replaceStr = L"\\r";
			size_t pos = writeWstr.find(searchStr);
			while (pos != std::wstring::npos) {
				writeWstr.replace(pos, 1, replaceStr);
				pos = writeWstr.find(searchStr, pos + replaceStr.length());
			}
			if (writeWstr.ends_with(L",]")) {  // 代哥的 Json 就是不一样
				writeWstr.erase(writeWstr.length() - 2, 1);
			}
			const auto dumpLocalFilePath = dumpBasePath / SCLocal::getFilePathByName(dumpName, true, dumpBasePath);
			std::ofstream dumpFile(dumpLocalFilePath, std::ofstream::out);
			dumpFile << utility::conversions::to_utf8string(writeWstr).c_str();
			printf("dump %ls success. (%ls)\n", dumpName.c_str(), dumpLocalFilePath.c_str());
			dumpedCount++;
			i++;
		}
		return dumpedCount;
	}

	std::string localizationDataCache("{}");
	int dumpAllScenarioData() {
		int totalCount = 0;
		const auto titleData = nlohmann::json::parse(localizationDataCache);
		for (auto& it : titleData.items()) {
			const auto& key = it.key();
			const std::string_view keyView(key);
			if (keyView.starts_with("mlADVInfo_Title")) {
				totalCount += dumpScenarioDataById(key.substr(16));
			}
		}
		return totalCount;
	}

	bool guiStarting = false;
	void startSCGUI() {
		if (guiStarting) return;
		guiStarting = true;
		std::thread([]() {
			printf("GUI START\n");
			guimain();
			guiStarting = false;
			printf("GUI END\n");
			}).detach();
	}

	void itLocalizationManagerDic(void* _this) {
		if (!SCGUIData::needExtractText) return;
		SCGUIData::needExtractText = false;

		static auto LocalizationManager_klass = il2cpp_symbols::get_class_from_instance(_this);
		static auto dic_field = il2cpp_class_get_field_from_name(LocalizationManager_klass, "dic");
		auto dic = il2cpp_symbols::read_field(_this, dic_field);

		static auto toJsonStr = reinterpret_cast<Il2CppString * (*)(void*)>(
			il2cpp_symbols::get_method_pointer("Newtonsoft.Json.dll", "Newtonsoft.Json",
				"JsonConvert", "SerializeObject", 1)
			);

		auto jsonStr = toJsonStr(dic);
		printf("dump text...\n");
		std::wstring jsonWStr(jsonStr->start_char);

		if (!std::filesystem::is_directory(dumpBasePath)) {
			std::filesystem::create_directories(dumpBasePath);
		}
		std::ofstream file(dumpBasePath / "localify.json", std::ofstream::out);
		const auto writeStr = utility::conversions::to_utf8string(jsonWStr);
		localizationDataCache = writeStr;
		file << writeStr.c_str();
		file.close();
		printf("write done.\n");

		const auto dumpCount = dumpAllScenarioData();
		printf("Dump finished. Total %d files\n", dumpCount);

		return;
	}

	void updateDicText(void* _this, Il2CppString* category, int id, std::string& newStr) {
		static auto LocalizationManager_klass = il2cpp_symbols::get_class_from_instance(_this);
		static auto dic_field = il2cpp_class_get_field_from_name(LocalizationManager_klass, "dic");
		auto dic = il2cpp_symbols::read_field(_this, dic_field);

		static auto dic_klass = il2cpp_symbols::get_class_from_instance(dic);
		static auto dict_get_Item = reinterpret_cast<void* (*)(void*, void*)>(
			il2cpp_class_get_method_from_name(dic_klass, "get_Item", 1)->methodPointer
			);

		auto categoryDict = dict_get_Item(dic, category);
		static auto cdic_klass = il2cpp_symbols::get_class_from_instance(categoryDict);
		static auto dict_set_Item = reinterpret_cast<void* (*)(void*, int, void*)>(
			il2cpp_class_get_method_from_name(cdic_klass, "set_Item", 2)->methodPointer
			);
		dict_set_Item(categoryDict, id, il2cpp_string_new(newStr.c_str()));

	}

	void* LocalizationManager_GetTextOrNull_orig;
	Il2CppString* LocalizationManager_GetTextOrNull_hook(void* _this, Il2CppString* category, int id) {
		if (g_max_fps != -1) set_fps_hook(g_max_fps);
		if (g_vsync_count != -1) set_vsync_count_hook(g_vsync_count);
		itLocalizationManagerDic(_this);
		std::string resultText = "";
		if (SCLocal::getLocalifyText(utility::conversions::to_utf8string(category->start_char), id, &resultText)) {
			//updateDicText(_this, category, id, resultText);
			return il2cpp_string_new(resultText.c_str());
		}
		// GG category: mlPublicText_GameGuard, id: 1
		return reinterpret_cast<decltype(LocalizationManager_GetTextOrNull_hook)*>(LocalizationManager_GetTextOrNull_orig)(_this, category, id);
	}

	void* get_NeedsLocalization_orig;
	bool get_NeedsLocalization_hook(void* _this) {
		auto ret = reinterpret_cast<decltype(get_NeedsLocalization_hook)*>(get_NeedsLocalization_orig)(_this);
		//printf("get_NeedsLocalization: %d\n", ret);
		return ret;
	}

	void* LiveMVView_UpdateLyrics_orig;
	void LiveMVView_UpdateLyrics_hook(void* _this, Il2CppString* text) {
		const std::wstring origWstr(text->start_char);
		const auto newText = SCLocal::getLyricsTrans(origWstr);
		reinterpret_cast<decltype(LiveMVView_UpdateLyrics_hook)*>(LiveMVView_UpdateLyrics_orig)(_this, il2cpp_string_new(newText.c_str()));
	}

	void* TMP_Text_set_text_orig;
	void TMP_Text_set_text_hook(void* _this, Il2CppString* value) {
		if (needPrintStack) {
			//wprintf(L"TMP_Text_set_text: %ls\n", value->start_char);
			//printf("%ls\n\n", environment_get_stacktrace()->start_char);
		}
		//if(value) value = il2cpp_symbols::NewWStr(std::format(L"(h){}", std::wstring(value->start_char)));
		reinterpret_cast<decltype(TMP_Text_set_text_hook)*>(TMP_Text_set_text_orig)(
			_this, value
			);
	}

	void* (*TMP_FontAsset_CreateFontAsset)(void* font);
	void (*TMP_Text_set_font)(void* _this, void* fontAsset);
	void* (*TMP_Text_get_font)(void* _this);

	void* UITextMeshProUGUI_Awake_orig;
	void UITextMeshProUGUI_Awake_hook(void* _this) {
		static auto get_Text = reinterpret_cast<Il2CppString * (*)(void*)>(
			il2cpp_symbols::get_method_pointer(
				"ENTERPRISE.UI.dll", "ENTERPRISE.UI",
				"UITextMeshProUGUI", "get_text", 0
			)
			);
		static auto set_Text = reinterpret_cast<void (*)(void*, Il2CppString*)>(
			il2cpp_symbols::get_method_pointer(
				"ENTERPRISE.UI.dll", "ENTERPRISE.UI",
				"UITextMeshProUGUI", "set_text", 1
			)
			);

		static auto get_LocalizeOnAwake = reinterpret_cast<bool (*)(void*)>(
			il2cpp_symbols::get_method_pointer(
				"ENTERPRISE.UI.dll", "ENTERPRISE.UI",
				"UITextMeshProUGUI", "get_LocalizeOnAwake", 0
			)
			);

		auto origText = get_Text(_this);
		if (origText) {
			// wprintf(L"UITextMeshProUGUI_Awake: %ls\n", origText->start_char);
			if (!get_LocalizeOnAwake(_this)) {
				std::string newTrans("");
				if (SCLocal::getGameUnlocalTrans(std::wstring(origText->start_char), &newTrans)) {
					set_Text(_this, il2cpp_string_new(newTrans.c_str()));
				}
			}
			else {
				// set_Text(_this, il2cpp_symbols::NewWStr(std::format(L"(接口l){}", std::wstring(origText->start_char))));
			}
		}

		static auto set_sourceFontFile = reinterpret_cast<void (*)(void*, void* font)>(
			il2cpp_symbols::get_method_pointer("Unity.TextMeshPro.dll", "TMPro",
				"TMP_FontAsset", "set_sourceFontFile", 1)
			);

		auto replaceFont = getReplaceFont();
		if (replaceFont) {
			auto origFont = TMP_Text_get_font(_this);
			set_sourceFontFile(origFont, replaceFont);
		}
		reinterpret_cast<decltype(UITextMeshProUGUI_Awake_hook)*>(UITextMeshProUGUI_Awake_orig)(_this);
	}

	void* ScenarioManager_Init_orig;
	void* ScenarioManager_Init_hook(void* retstr, void* _this, Il2CppString* scrName) {
		// printf("ScenarioManager_Init: %ls\n%ls\n\n", scrName->start_char, environment_get_stacktrace()->start_char);
		return reinterpret_cast<decltype(ScenarioManager_Init_hook)*>(ScenarioManager_Init_orig)(retstr, _this, scrName);
	}

	void* (*ReadAllBytes)(Il2CppString*);
	void* readFileAllBytes(const std::wstring& name) {
		return ReadAllBytes(il2cpp_symbols::NewWStr(name));
	}

	void* DataFile_GetBytes_hook(Il2CppString* path) {
		std::wstring pathStr(path->start_char);
		std::wstring_view pathStrView(pathStr);
		//wprintf(L"DataFile_GetBytes: %ls\n", pathStr.c_str());

		
		std::filesystem::path localFileName;
		if (SCLocal::getLocalFileName(pathStr, &localFileName)) {
			return readFileAllBytes(localFileName);
		}

		auto ret = reinterpret_cast<decltype(DataFile_GetBytes_hook)*>(DataFile_GetBytes_orig)(path);
		if (g_auto_dump_all_json) {
			if (pathStrView.ends_with(L".json")) {
				auto basePath = std::filesystem::path("dumps") / "autoDumpJson";
				std::filesystem::path fileName = basePath / SCLocal::getFilePathByName(pathStr, true, basePath);
				
				if (!std::filesystem::is_directory(basePath)) {
					std::filesystem::create_directories(basePath);
				}
				auto newStr = bytesToIl2cppString(ret);
				std::ofstream fileDump(fileName, std::ofstream::out);
				auto writeWstr = std::wstring(newStr->start_char);

				const std::wstring searchStr = L"\r";
				const std::wstring replaceStr = L"\\r";
				size_t pos = writeWstr.find(searchStr);
				while (pos != std::wstring::npos) {
					writeWstr.replace(pos, 1, replaceStr);
					pos = writeWstr.find(searchStr, pos + replaceStr.length());
				}

				fileDump << utility::conversions::to_utf8string(writeWstr).c_str();
				fileDump.close();
				printf("Auto dump: %ls\n", fileName.c_str());
			}
		}
		return ret;
	}

	void SetResolution_hook(int width, int height, bool fullscreen) {
		return;
		/*
		width = SCGUIData::screenW;
		height = SCGUIData::screenH;
		fullscreen = SCGUIData::screenFull;
		return reinterpret_cast<decltype(SetResolution_hook)*>(SetResolution_orig)(width, height, fullscreen);
		*/
	}

	void* InvokeMoveNext_orig;
	void InvokeMoveNext_hook(void* enumerator, void* returnValueAddress) {
		MHotkey::start_hotkey(hotKey);
		return reinterpret_cast<decltype(InvokeMoveNext_hook)*>(InvokeMoveNext_orig)(enumerator, returnValueAddress);
	}

	void* Live_SetEnableDepthOfField_orig;
	void Live_SetEnableDepthOfField_hook(void* _this, bool isEnable) {
		if (g_enable_free_camera) {
			isEnable = false;
		}
		return reinterpret_cast<decltype(Live_SetEnableDepthOfField_hook)*>(Live_SetEnableDepthOfField_orig)(_this, isEnable);
	}

	void* Live_Update_orig;
	void Live_Update_hook(void* _this) {
		reinterpret_cast<decltype(Live_Update_hook)*>(Live_Update_orig)(_this);
		if (g_enable_free_camera) {
			Live_SetEnableDepthOfField_hook(_this, false);
		}
	}

	void* Decrypt_File_orig;
	Il2CppString* Decrypt_File_hook(Il2CppString* target, Il2CppString* password, Il2CppString* salt) {
		Il2CppString* result = reinterpret_cast<decltype(Decrypt_File_hook)*>(Decrypt_File_orig)(target, password, salt);
		/*wprintf(L"Decrypt_File_hook: target: %ls, password: %ls, salt: %ls, return: %ls\n", 
			target->start_char, 
			password->start_char, 
			salt->start_char, 
			result->start_char);*/
		return result;
	}

	void* Sqlite_BackUp_orig;
	void Sqlite_BackUp_hook(void* _this, Il2CppString* destinationDatabasePath, Il2CppString* databaseName) {
		reinterpret_cast<decltype(Sqlite_BackUp_hook)*>(Sqlite_BackUp_orig)(_this, destinationDatabasePath, databaseName);
	}


	std::vector<void*> instances;
	void BackUpFunc() {
		for (void* db : instances) {
			const auto path = L"C:/Users/long1/Desktop/";
			const auto path_str = il2cpp_symbols::NewWStr(path);
			const auto name = L"meta.sql";
			const auto name_str = il2cpp_symbols::NewWStr(path);
			wprintf(L"####Create BackUp \n");
			Sqlite_BackUp_hook(db, path_str, name_str);
		}
	}

	void* Sqlite_Set_orig;
	void Sqlite_Set_hook(void* _this, Il2CppString* value) {
		//wprintf(L"Sqlite_Set_hook: value: %ls\n", value->start_char);
		reinterpret_cast<decltype(Sqlite_Set_hook)*>(Sqlite_Set_orig)(_this, value);
	}

	void* Sqlite_Ctor_orig;
	void Sqlite_Ctor_hook(void* _this, void* connectionString) {
		wprintf(L"Sqlite_Ctor_hook\n");
		reinterpret_cast<decltype(Sqlite_Ctor_hook)*>(Sqlite_Ctor_orig)(_this, connectionString);
	}

	void* Sqlite_Close_orig;
	void Sqlite_Close_hook(void* _this) {
		wprintf(L"Sqlite_Close_hook\n");
		reinterpret_cast<decltype(Sqlite_Close_hook)*>(Sqlite_Close_orig)(_this);
	}

	void* Sqlite_Execute_orig;
	int Sqlite_Execute_hook(void* _this, Il2CppString* query, void* args) {
		wprintf(L"Sqlite_Execute_hook: query: %ls\n", query->start_char);
		return reinterpret_cast<decltype(Sqlite_Execute_hook)*>(Sqlite_Execute_orig)(_this, query, args);
	}

	void* Sqlite_Commit_orig;
	void Sqlite_Commit_hook(void* _this) {
		wprintf(L"Sqlite_Commit_hook\n");
		reinterpret_cast<decltype(Sqlite_Commit_hook)*>(Sqlite_Commit_orig)(_this);
	}

	void* Write_Byte_orig;
	void Write_Byte_hook(Il2CppString* path, void* bytes) {
		reinterpret_cast<decltype(Write_Byte_hook)*>(Write_Byte_orig)(path, bytes);
	}

	void* Decrypt_Aes_orig;
	void Decrypt_Aes_hook(void* _this, void* indata, void* outdata, unsigned* ekey) {
		reinterpret_cast<decltype(Decrypt_Aes_hook)*>(Decrypt_Aes_orig)(_this, indata, outdata, ekey);
		//wprintf(L"Decrypt_Aes_hook\n");
	}

	void* Decrypt_Key_orig;
	void Decrypt_Key_hook(void* _this, void* value) {
		reinterpret_cast<decltype(Decrypt_Key_hook)*>(Decrypt_Key_orig)(_this, value);
		//wprintf(L"Decrypt_Key_hook key\n");
		const auto path = L"C:/Users/long1/Desktop/key.txt";
		const auto path_str = il2cpp_symbols::NewWStr(path);
		Write_Byte_hook(path_str, value);
	}

	void* Decrypt_IV_orig;
	void Decrypt_IV_hook(void* _this, void* value) {
		reinterpret_cast<decltype(Decrypt_IV_hook)*>(Decrypt_IV_orig)(_this, value);
		//wprintf(L"Decrypt_IV_hook key\n");
		const auto path = L"C:/Users/long1/Desktop/iv.txt";
		const auto path_str = il2cpp_symbols::NewWStr(path);
		Write_Byte_hook(path_str, value);
	}

	void* Decrypt_Padding_orig;
	void Decrypt_Padding_hook(void* _this, int value) {
		reinterpret_cast<decltype(Decrypt_Padding_hook)*>(Decrypt_Padding_orig)(_this, value);
		//wprintf(L"Decrypt_Padding_hook key %d\n", value);
	}

	void* Decrypt_Mode_orig;
	void Decrypt_Mode_hook(void* _this, int value) {
		reinterpret_cast<decltype(Decrypt_Mode_hook)*>(Decrypt_Mode_orig)(_this, value);
		//wprintf(L"Decrypt_Mode_hook key %d\n", value);
	}

	void* CriWareErrorHandler_HandleMessage_orig;
	void CriWareErrorHandler_HandleMessage_hook(void* _this, Il2CppString* msg) {
		// wprintf(L"CriWareErrorHandler_HandleMessage: %ls\n%ls\n\n", msg->start_char, environment_get_stacktrace()->start_char);
		environment_get_stacktrace();
		return reinterpret_cast<decltype(CriWareErrorHandler_HandleMessage_hook)*>(CriWareErrorHandler_HandleMessage_orig)(_this, msg);
	}

	void* UnsafeLoadBytesFromKey_orig;
	void* UnsafeLoadBytesFromKey_hook(void* _this, Il2CppString* tagName, Il2CppString* assetKey) {
		//wprintf(L"UnsafeLoadBytesFromKey: tag: %ls, assetKey: %ls\n", tagName->start_char, assetKey->start_char);
		return reinterpret_cast<decltype(UnsafeLoadBytesFromKey_hook)*>(UnsafeLoadBytesFromKey_orig)(_this, tagName, assetKey);
	}

	void* baseCameraTransform = nullptr;
	void* baseCamera = nullptr;

	void* Unity_set_pos_injected_orig;
	void Unity_set_pos_injected_hook(void* _this, Vector3_t* ret) {
		return reinterpret_cast<decltype(Unity_set_pos_injected_hook)*>(Unity_set_pos_injected_orig)(_this, ret);
	}

	void* Unity_get_pos_injected_orig;
	void Unity_get_pos_injected_hook(void* _this, Vector3_t* data) {
		reinterpret_cast<decltype(Unity_get_pos_injected_hook)*>(Unity_get_pos_injected_orig)(_this, data);

		if (_this == baseCameraTransform) {
			if (guiStarting) {
				SCGUIData::sysCamPos.x = data->x;
				SCGUIData::sysCamPos.y = data->y;
				SCGUIData::sysCamPos.z = data->z;
			}
			if (g_enable_free_camera) {
				SCCamera::baseCamera.updateOtherPos(data);
				Unity_set_pos_injected_hook(_this, data);
			}
		}
	}

	void* Unity_set_fieldOfView_orig;
	void Unity_set_fieldOfView_hook(void* _this, float single) {
		return reinterpret_cast<decltype(Unity_set_fieldOfView_hook)*>(Unity_set_fieldOfView_orig)(_this, single);
	}
	void* Unity_get_fieldOfView_orig;
	float Unity_get_fieldOfView_hook(void* _this) {
		const auto origFov = reinterpret_cast<decltype(Unity_get_fieldOfView_hook)*>(Unity_get_fieldOfView_orig)(_this);
		if (_this == baseCamera) {
			if (guiStarting) {
				SCGUIData::sysCamFov = origFov;
			}
			if (g_enable_free_camera) {
				const auto fov = SCCamera::baseCamera.fov;
				Unity_set_fieldOfView_hook(_this, fov);
				return fov;
			}
		}
		// printf("get_fov: %f\n", ret);
		return origFov;
	}

	void* Unity_LookAt_Injected_orig;
	void Unity_LookAt_Injected_hook(void* _this, Vector3_t* worldPosition, Vector3_t* worldUp) {
		if (_this == baseCameraTransform) {
			if (g_enable_free_camera) {
				auto pos = SCCamera::baseCamera.getLookAt();
				worldPosition->x = pos.x;
				worldPosition->y = pos.y;
				worldPosition->z = pos.z;
			}
		}
		return reinterpret_cast<decltype(Unity_LookAt_Injected_hook)*>(Unity_LookAt_Injected_orig)(_this, worldPosition, worldUp);
	}
	void* Unity_set_nearClipPlane_orig;
	void Unity_set_nearClipPlane_hook(void* _this, float single) {
		if (_this == baseCamera) {
			if (g_enable_free_camera) {
				single = 0.001f;
			}
		}
		return reinterpret_cast<decltype(Unity_set_nearClipPlane_hook)*>(Unity_set_nearClipPlane_orig)(_this, single);
	}

	void* Unity_get_nearClipPlane_orig;
	float Unity_get_nearClipPlane_hook(void* _this) {
		auto ret = reinterpret_cast<decltype(Unity_get_nearClipPlane_hook)*>(Unity_get_nearClipPlane_orig)(_this);
		if (_this == baseCamera) {
			if (g_enable_free_camera) {
				ret = 0.001f;
			}
		}
		return ret;
	}
	void* Unity_get_farClipPlane_orig;
	float Unity_get_farClipPlane_hook(void* _this) {
		auto ret = reinterpret_cast<decltype(Unity_get_farClipPlane_hook)*>(Unity_get_farClipPlane_orig)(_this);
		if (_this == baseCamera) {
			if (g_enable_free_camera) {
				ret = 2500.0f;
			}
		}
		return ret;
	}

	void* Unity_set_farClipPlane_orig;
	void Unity_set_farClipPlane_hook(void* _this, float value) {
		if(_this == baseCamera) {
			if (g_enable_free_camera) {
				value = 2500.0f;
			}
		}
		reinterpret_cast<decltype(Unity_set_farClipPlane_hook)*>(Unity_set_farClipPlane_orig)(_this, value);
	}

	void* Unity_set_rotation_Injected_orig;
	void Unity_set_rotation_Injected_hook(void* _this, Quaternion_t* value) {
		return reinterpret_cast<decltype(Unity_set_rotation_Injected_hook)*>(Unity_set_rotation_Injected_orig)(_this, value);
	}
	void* Unity_get_rotation_Injected_orig;
	void Unity_get_rotation_Injected_hook(void* _this, Quaternion_t* ret) {
		reinterpret_cast<decltype(Unity_get_rotation_Injected_hook)*>(Unity_get_rotation_Injected_orig)(_this, ret);

		if (_this == baseCameraTransform) {
			if (guiStarting) {
				SCGUIData::sysCamRot.w = ret->w;
				SCGUIData::sysCamRot.x = ret->x;
				SCGUIData::sysCamRot.y = ret->y;
				SCGUIData::sysCamRot.z = ret->z;
				SCGUIData::updateSysCamLookAt();
			}
			if (g_enable_free_camera) {
				ret->w = 0;
				ret->x = 0;
				ret->y = 0;
				ret->z = 0;
				Unity_set_rotation_Injected_hook(_this, ret);

				static auto Vector3_klass = il2cpp_symbols::get_class("UnityEngine.CoreModule.dll", "UnityEngine", "Vector3");
				Vector3_t* pos = reinterpret_cast<Vector3_t*>(il2cpp_object_new(Vector3_klass));
				Vector3_t* up = reinterpret_cast<Vector3_t*>(il2cpp_object_new(Vector3_klass));
				up->x = 0;
				up->y = 1;
				up->z = 0;
				Unity_LookAt_Injected_hook(_this, pos, up);
			}
		}
	}


	void* get_baseCamera_orig;
	void* get_baseCamera_hook(void* _this) {
		// printf("get_baseCamera\n");
		auto ret = reinterpret_cast<decltype(get_baseCamera_hook)*>(get_baseCamera_orig)(_this);  // UnityEngine.Camera

		if (g_enable_free_camera || guiStarting) {
			static auto GetComponent = reinterpret_cast<void* (*)(void*, Il2CppReflectionType*)>(
				il2cpp_symbols::get_method_pointer("UnityEngine.CoreModule.dll", "UnityEngine",
					"Component", "GetComponent", 1));

			static auto transClass = il2cpp_symbols::get_class("UnityEngine.CoreModule.dll", "UnityEngine", "Transform");
			static auto transType = il2cpp_type_get_object(il2cpp_class_get_type(transClass));

			auto transform = GetComponent(ret, transType);  // UnityEngine.Transform
			if (transform) {
				baseCameraTransform = transform;
				baseCamera = ret;
			}
		}

		return ret;
	}

	void readDMMGameGuardData();

	void* GGIregualDetector_ShowPopup_orig;
	void GGIregualDetector_ShowPopup_hook(void* _this, int code) {
		//printf("GGIregualDetector_ShowPopup: code: %d\n%ls\n\n", code, environment_get_stacktrace()->start_char);
		//readDMMGameGuardData();

		static auto this_klass = il2cpp_symbols::get_class_from_instance(_this);
		static FieldInfo* isShowPopup_field = il2cpp_class_get_field_from_name(this_klass, "_isShowPopup");
		static FieldInfo* isError_field = il2cpp_class_get_field_from_name(this_klass, "_isError");
		auto fieldData = il2cpp_class_get_static_field_data(this_klass);
		auto isError = il2cpp_symbols::read_field<bool>(fieldData, isError_field);

		if (isError) {
			// if (code == 0) return;
		}

		return reinterpret_cast<decltype(GGIregualDetector_ShowPopup_hook)*>(GGIregualDetector_ShowPopup_orig)(_this, code);
	}

	void* DMMGameGuard_NPGameMonCallback_orig;
	int DMMGameGuard_NPGameMonCallback_hook(UINT dwMsg, UINT dwArg) {
		// printf("DMMGameGuard_NPGameMonCallback: dwMsg: %u, dwArg: %u\n%ls\n\n", dwMsg, dwArg, environment_get_stacktrace()->start_char);
		return 1;
		// return reinterpret_cast<decltype(DMMGameGuard_NPGameMonCallback_hook)*>(DMMGameGuard_NPGameMonCallback_orig)(dwMsg, dwArg);
	}

	void* set_vsync_count_orig;
	void set_vsync_count_hook(int value) {
		return reinterpret_cast<decltype(set_vsync_count_hook)*>(set_vsync_count_orig)(g_vsync_count == -1 ? value : g_vsync_count);
	}

	void* set_fps_orig;
	void set_fps_hook(int value) {
		return reinterpret_cast<decltype(set_fps_hook)*>(set_fps_orig)(g_max_fps == -1 ? value : g_max_fps);
	}

	void* Unity_Quit_orig;
	void Unity_Quit_hook(int code) {
		printf("Quit code: %d\n", code);
		// printf("Quit code: %d\n%ls\n\n", code, environment_get_stacktrace()->start_char);
	}

	void* SetCallbackToGameMon_orig;
	int SetCallbackToGameMon_hook(void* DMMGameGuard_NPGameMonCallback_addr) {
		// printf("SetCallbackToGameMon: %p\n%ls\n\n", DMMGameGuard_NPGameMonCallback_addr, environment_get_stacktrace()->start_char);
		ADD_HOOK(DMMGameGuard_NPGameMonCallback, "DMMGameGuard_NPGameMonCallback at %p");
		return reinterpret_cast<decltype(SetCallbackToGameMon_hook)*>(SetCallbackToGameMon_orig)(DMMGameGuard_NPGameMonCallback_addr);
	}

	void* DMMGameGuard_Setup_orig;
	void DMMGameGuard_Setup_hook() {
		// printf("\n\nDMMGameGuard_Setup\n\n");
		readDMMGameGuardData();
	}

	void* DMMGameGuard_SetCheckMode_orig;
	void DMMGameGuard_SetCheckMode_hook(bool isCheck) {
		// printf("DMMGameGuard_SetCheckMode before\n");
		// readDMMGameGuardData();
		reinterpret_cast<decltype(DMMGameGuard_SetCheckMode_hook)*>(DMMGameGuard_SetCheckMode_orig)(isCheck);
		// printf("DMMGameGuard_SetCheckMode after\n");
		// readDMMGameGuardData();
	}

	void* PreInitNPGameMonW_orig;
	void PreInitNPGameMonW_hook(int64_t v) {
		// printf("PreInitNPGameMonW\n");
		// readDMMGameGuardData();
	}
	void* PreInitNPGameMonA_orig;
	void PreInitNPGameMonA_hook(int64_t v) {
		// printf("PreInitNPGameMonA\n");
		// readDMMGameGuardData();
	}

	void* InitNPGameMon_orig;
	UINT InitNPGameMon_hook() {
		// printf("InitNPGameMon\n");
		// readDMMGameGuardData();
		return 1877;
	}

	void* SetHwndToGameMon_orig;
	void SetHwndToGameMon_hook(int64_t v) {
		// printf("SetHwndToGameMon\n");
		// readDMMGameGuardData();
	}

	void* DMMGameGuard_klass;
	FieldInfo* isCheck_field;
	FieldInfo* isInit_field;
	FieldInfo* bAppExit_field;
	FieldInfo* errCode_field;

	void readDMMGameGuardData() {
		if (!DMMGameGuard_klass) return;
		if (!isCheck_field) {
			printf("isCheck_field invalid: %p\n", isCheck_field);
			return;
		}

		auto staticData = il2cpp_class_get_static_field_data(DMMGameGuard_klass);
		auto isCheck = il2cpp_symbols::read_field<bool>(staticData, isCheck_field);
		auto isInit = il2cpp_symbols::read_field<bool>(staticData, isInit_field);
		auto bAppExit = il2cpp_symbols::read_field<bool>(staticData, bAppExit_field);
		auto errCode = il2cpp_symbols::read_field<int>(staticData, errCode_field);
		if (bAppExit) {
			il2cpp_symbols::write_field(staticData, bAppExit_field, false);
		}

		printf("DMMGameGuardData: isCheck: %d, isInit: %d, bAppExit: %d, errCode: %d\n\n", isCheck, isInit, bAppExit, errCode);
	}

	bool pathed = false;
	bool npPatched = false;
	
	void patchNP(HMODULE module) {
		if (npPatched) return;
		npPatched = true;
		// auto np_module = GetModuleHandle("NPGameDLL");
		if (module) {
			auto SetCallbackToGameMon_addr = GetProcAddress(module, "SetCallbackToGameMon");
			auto PreInitNPGameMonW_addr = GetProcAddress(module, "PreInitNPGameMonW");
			auto PreInitNPGameMonA_addr = GetProcAddress(module, "PreInitNPGameMonA");
			auto InitNPGameMon_addr = GetProcAddress(module, "InitNPGameMon");
			auto SetHwndToGameMon_addr = GetProcAddress(module, "SetHwndToGameMon");

			ADD_HOOK(SetCallbackToGameMon, "SetCallbackToGameMon ap %p");
			ADD_HOOK(PreInitNPGameMonW, "PreInitNPGameMonW ap %p");
			ADD_HOOK(PreInitNPGameMonA, "PreInitNPGameMonA ap %p");
			ADD_HOOK(InitNPGameMon, "InitNPGameMon ap %p");
			ADD_HOOK(SetHwndToGameMon, "SetHwndToGameMon ap %p");
		}
		else {
			printf("NPGameDLL Get failed.\n");
		}
	}

	int retryCount = 0;
	void path_game_assembly()
	{
		if (!mh_inited)
			return;

		auto il2cpp_module = GetModuleHandle("GameAssembly.dll");
		if (!il2cpp_module) {
			printf("GameAssembly.dll not loaded.\n");
			retryCount++;
			if (retryCount == 15) {
				reopen_self();
				return;
			}
		}

		if (pathed) return;
		pathed = true;

		printf("Trying to patch GameAssembly.dll...\n");

		// load il2cpp exported functions
		il2cpp_symbols::init(il2cpp_module);

#pragma region HOOK_ADDRESSES
		
		environment_get_stacktrace = reinterpret_cast<decltype(environment_get_stacktrace)>(
			il2cpp_symbols::get_method_pointer("mscorlib.dll", "System", 
				"Environment", "get_StackTrace", 0)
			);

		DMMGameGuard_klass = il2cpp_symbols::get_class("PRISM.Legacy.dll", "PRISM", "DMMGameGuard");
		isCheck_field = il2cpp_class_get_field_from_name(DMMGameGuard_klass, "_isCheck");
		isInit_field = il2cpp_class_get_field_from_name(DMMGameGuard_klass, "_isInit");
		bAppExit_field = il2cpp_class_get_field_from_name(DMMGameGuard_klass, "_bAppExit");
		errCode_field = il2cpp_class_get_field_from_name(DMMGameGuard_klass, "_errCode");
		initEncoding();
		ReadAllBytes = reinterpret_cast<decltype(ReadAllBytes)>(
			il2cpp_symbols::get_method_pointer("mscorlib.dll", "System.IO", "File", "ReadAllBytes", 1)
			);

		AssetBundle_LoadFromFile = reinterpret_cast<decltype(AssetBundle_LoadFromFile)>(
			il2cpp_symbols::get_method_pointer(
				"UnityEngine.AssetBundleModule.dll", "UnityEngine",
				"AssetBundle", "LoadFromFile", 3
			)
			);
		Object_IsNativeObjectAlive = reinterpret_cast<bool(*)(void*)>(
			il2cpp_symbols::get_method_pointer("UnityEngine.CoreModule.dll", "UnityEngine",
				"Object", "IsNativeObjectAlive", 1)
			);
		AssetBundle_LoadAsset = reinterpret_cast<decltype(AssetBundle_LoadAsset)>(
			il2cpp_resolve_icall("UnityEngine.AssetBundle::LoadAsset_Internal(System.String,System.Type)")
			);
		const auto FontClass = il2cpp_symbols::get_class("UnityEngine.TextRenderingModule.dll", "UnityEngine", "Font");
		Font_Type = il2cpp_type_get_object(il2cpp_class_get_type(FontClass));
		TMP_FontAsset_CreateFontAsset = reinterpret_cast<decltype(TMP_FontAsset_CreateFontAsset)>(il2cpp_symbols::get_method_pointer(
			"Unity.TextMeshPro.dll", "TMPro",
			"TMP_FontAsset", "CreateFontAsset", 1
		));
		TMP_Text_set_font = reinterpret_cast<decltype(TMP_Text_set_font)>(il2cpp_symbols::get_method_pointer(
			"Unity.TextMeshPro.dll", "TMPro",
			"TMP_Text", "set_font", 1
		));
		TMP_Text_get_font = reinterpret_cast<decltype(TMP_Text_get_font)>(il2cpp_symbols::get_method_pointer(
			"Unity.TextMeshPro.dll", "TMPro",
			"TMP_Text", "get_font", 0
		));
		
		const auto TMP_Text_set_text_addr = il2cpp_symbols::get_method_pointer(
			"Unity.TextMeshPro.dll", "TMPro",
			"TMP_Text", "set_text", 1
		);
		const auto UITextMeshProUGUI_Awake_addr = il2cpp_symbols::get_method_pointer(
			"ENTERPRISE.UI.dll", "ENTERPRISE.UI",
			"UITextMeshProUGUI", "Awake", 0
		);

		auto ScenarioManager_Init_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "PRISM.Scenario",
			"ScenarioManager", "Init", 1
		);
		auto DataFile_GetBytes_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "PRISM",
			"DataFile", "GetBytes", 1
		);
		DataFile_IsKeyExist = reinterpret_cast<decltype(DataFile_IsKeyExist)>(
			il2cpp_symbols::get_method_pointer(
				"PRISM.Legacy.dll", "PRISM",
				"DataFile", "IsKeyExist", 1
			)
			);

		auto SetResolution_addr = il2cpp_symbols::get_method_pointer(
			"UnityEngine.CoreModule.dll", "UnityEngine",
			"Screen", "SetResolution", 3
		);

		auto UnsafeLoadBytesFromKey_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.ResourceManagement.dll", "PRISM.ResourceManagement",
			"ResourceLoader", "UnsafeLoadBytesFromKey", 2
		);

		auto LocalizationManager_GetTextOrNull_addr = il2cpp_symbols::get_method_pointer(
			"ENTERPRISE.Localization.dll", "ENTERPRISE.Localization",
			"LocalizationManager", "GetTextOrNull", 2
		);
		auto get_NeedsLocalization_addr = il2cpp_symbols::get_method_pointer(
			"ENTERPRISE.UI.dll", "ENTERPRISE.UI",
			"UITextMeshProUGUI", "get_NeedsLocalization", 0
		);
		auto LiveMVView_UpdateLyrics_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "PRISM.Live",
			"LiveMVView", "UpdateLyrics", 1
		);

		auto get_baseCamera_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "PRISM",
			"CameraController", "get_baseCamera", 0
		);

		auto Unity_get_pos_injected_addr = il2cpp_resolve_icall("UnityEngine.Transform::get_position_Injected(UnityEngine.Vector3&)");
		auto Unity_set_pos_injected_addr = il2cpp_resolve_icall("UnityEngine.Transform::set_position_Injected(UnityEngine.Vector3&)");

		auto Unity_get_fieldOfView_addr = il2cpp_resolve_icall("UnityEngine.Camera::get_fieldOfView()");
		auto Unity_set_fieldOfView_addr = il2cpp_resolve_icall("UnityEngine.Camera::set_fieldOfView(System.Single)");
		auto Unity_LookAt_Injected_addr = il2cpp_resolve_icall("UnityEngine.Transform::Internal_LookAt_Injected(UnityEngine.Vector3&,UnityEngine.Vector3&)");
		auto Unity_set_nearClipPlane_addr = il2cpp_resolve_icall("UnityEngine.Camera::set_nearClipPlane(System.Single)");
		auto Unity_get_nearClipPlane_addr = il2cpp_resolve_icall("UnityEngine.Camera::get_nearClipPlane()");
		auto Unity_get_farClipPlane_addr = il2cpp_resolve_icall("UnityEngine.Camera::get_farClipPlane()");
		auto Unity_set_farClipPlane_addr = il2cpp_resolve_icall("UnityEngine.Camera::set_farClipPlane(System.Single)");
		auto Unity_get_rotation_Injected_addr = il2cpp_resolve_icall("UnityEngine.Transform::get_rotation_Injected(UnityEngine.Quaternion&)");
		auto Unity_set_rotation_Injected_addr = il2cpp_resolve_icall("UnityEngine.Transform::set_rotation_Injected(UnityEngine.Quaternion&)");

		auto InvokeMoveNext_addr = il2cpp_symbols::get_method_pointer(
			"UnityEngine.CoreModule.dll", "UnityEngine",
			"SetupCoroutine", "InvokeMoveNext", 2
		);
		auto Live_SetEnableDepthOfField_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "PRISM",
			"LiveScene", "SetEnableDepthOfField", 1
		);
		auto Live_Update_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "PRISM",
			"LiveScene", "Update", 0
		);
		auto Decrypt_File_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "ENTERPRISE.OutGame",
			"EncryptionUtility", "Decrypt", 3
		);
		auto Sqlite_BackUp_addr = il2cpp_symbols::get_method_pointer(
			"Emberbox.dll", "SQLite",
			"SQLiteConnection", "Backup", 2
		);
		auto Sqlite_Set_addr = il2cpp_symbols::get_method_pointer(
			"Emberbox.dll", "SQLite",
			"SQLiteConnection", "set_DatabasePath", 1
		);
		auto Sqlite_Ctor_addr = il2cpp_symbols::get_method_pointer(
			"Emberbox.dll", "SQLite",
			"SQLiteConnection", ".ctor", 1
		);
		auto Sqlite_Close_addr = il2cpp_symbols::get_method_pointer(
			"Emberbox.dll", "SQLite",
			"SQLiteConnection", "Close", 0
		);
		auto Sqlite_Execute_addr = il2cpp_symbols::get_method_pointer(
			"Emberbox.dll", "SQLite",
			"SQLiteConnection", "Execute", 2
		);
		auto Sqlite_Commit_addr = il2cpp_symbols::get_method_pointer(
			"Emberbox.dll", "SQLite",
			"SQLiteConnection", "Commit", 0
		);
		auto Decrypt_Aes_addr = il2cpp_symbols::get_method_pointer(
			"System.Core.dll", "System.Security.Cryptography",
			"AesTransform", "Decrypt128", 3
		);
		auto Decrypt_Key_addr = il2cpp_symbols::get_method_pointer(
			"System.Core.dll", "System.Security.Cryptography",
			"AesCryptoServiceProvider", "set_Key", 1
		);
		auto Decrypt_IV_addr = il2cpp_symbols::get_method_pointer(
			"System.Core.dll", "System.Security.Cryptography",
			"AesCryptoServiceProvider", "set_IV", 1
		);
		auto Decrypt_Padding_addr = il2cpp_symbols::get_method_pointer(
			"System.Core.dll", "System.Security.Cryptography",
			"AesCryptoServiceProvider", "set_Padding", 1
		);

		auto Decrypt_Mode_addr = il2cpp_symbols::get_method_pointer(
			"System.Core.dll", "System.Security.Cryptography",
			"AesCryptoServiceProvider", "set_Mode", 1
		);
		auto Write_Byte_addr = il2cpp_symbols::get_method_pointer(
			"mscorlib.dll", "System.IO",
			"File", "WriteAllBytes", 2
		);
		auto CriWareErrorHandler_HandleMessage_addr = il2cpp_symbols::get_method_pointer(
			"CriMw.CriWare.Runtime.dll", "CriWare",
			"CriWareErrorHandler", "HandleMessage", 1
		);

		auto GGIregualDetector_ShowPopup_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "PRISM",
			"GGIregualDetector", "ShowPopup", 1
		);
		auto DMMGameGuard_NPGameMonCallback_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "PRISM",
			"DMMGameGuard", "NPGameMonCallback", 2
		);
		auto DMMGameGuard_Setup_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "PRISM",
			"DMMGameGuard", "Setup", 0
		);		
		auto DMMGameGuard_SetCheckMode_addr = il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "PRISM",
			"DMMGameGuard", "SetCheckMode", 1
		);
		/*
		CloseNPGameMon = reinterpret_cast<decltype(CloseNPGameMon)>(il2cpp_symbols::get_method_pointer(
			"PRISM.Legacy.dll", "PRISM",
			"DMMGameGuard", "CloseNPGameMon", 0
		));*/

		auto set_fps_addr = il2cpp_resolve_icall("UnityEngine.Application::set_targetFrameRate(System.Int32)");
		auto set_vsync_count_addr = il2cpp_resolve_icall("UnityEngine.QualitySettings::set_vSyncCount(System.Int32)");
		auto Unity_Quit_addr = il2cpp_resolve_icall("UnityEngine.Application::Quit(System.Int32)");

#pragma endregion
		ADD_HOOK(LocalizationManager_GetTextOrNull, "LocalizationManager_GetTextOrNull at %p");
		ADD_HOOK(get_NeedsLocalization, "get_NeedsLocalization at %p");
		ADD_HOOK(LiveMVView_UpdateLyrics, "LiveMVView_UpdateLyrics at %p");
		ADD_HOOK(get_baseCamera, "get_baseCamera at %p");
		ADD_HOOK(Unity_set_pos_injected, "Unity_set_pos_injected at %p");
		ADD_HOOK(Unity_get_pos_injected, "Unity_get_pos_injected at %p");
		ADD_HOOK(Unity_get_fieldOfView, "Unity_get_fieldOfView at %p");
		ADD_HOOK(Unity_set_fieldOfView, "Unity_set_fieldOfView at %p");
		ADD_HOOK(Unity_LookAt_Injected, "Unity_LookAt_Injected at %p");
		ADD_HOOK(Unity_set_nearClipPlane, "Unity_set_nearClipPlane at %p");
		ADD_HOOK(Unity_get_nearClipPlane, "Unity_get_nearClipPlane at %p");
		ADD_HOOK(Unity_get_farClipPlane, "Unity_get_farClipPlane at %p");
		ADD_HOOK(Unity_set_farClipPlane, "Unity_set_farClipPlane at %p");
		ADD_HOOK(Unity_get_rotation_Injected, "Unity_get_rotation_Injected at %p");
		ADD_HOOK(Unity_set_rotation_Injected, "Unity_set_rotation_Injected at %p");

		ADD_HOOK(TMP_Text_set_text, "TMP_Text_set_text at %p");
		ADD_HOOK(UITextMeshProUGUI_Awake, "UITextMeshProUGUI_Awake at %p");
		ADD_HOOK(ScenarioManager_Init, "ScenarioManager_Init at %p");
		ADD_HOOK(DataFile_GetBytes, "DataFile_GetBytes at %p");
		ADD_HOOK(UnsafeLoadBytesFromKey, "UnsafeLoadBytesFromKey at %p");
		ADD_HOOK(SetResolution, "SetResolution at %p");
		ADD_HOOK(InvokeMoveNext, "InvokeMoveNext at %p");
		ADD_HOOK(Live_SetEnableDepthOfField, "Live_SetEnableDepthOfField at %p");
		ADD_HOOK(Live_Update, "Live_Update at %p");
		ADD_HOOK(Decrypt_File, "Decrypt_File at %p")
		ADD_HOOK(Sqlite_BackUp, "Sqlite_BackUp at %p")
		ADD_HOOK(Sqlite_Execute, "Sqlite_Execute at %p")
		ADD_HOOK(Sqlite_Commit, "Sqlite_Commit at %p")
		ADD_HOOK(Sqlite_Set, "Sqlite_Set at %p")
		ADD_HOOK(Sqlite_Ctor, "Sqlite_Ctor at %p")
		ADD_HOOK(Sqlite_Close, "Sqlite_Close at %p")
		ADD_HOOK(Write_Byte, "Write_Byte at %p")
		ADD_HOOK(Decrypt_Aes, "Decrypt_Aes at %p")
		ADD_HOOK(Decrypt_Key, "Decrypt_Key at %p")
		ADD_HOOK(Decrypt_IV, "Decrypt_IV at %p")
		ADD_HOOK(Decrypt_Padding, "Decrypt_Padding at %p")
		ADD_HOOK(Decrypt_Mode, "Decrypt_Mode at %p")
		ADD_HOOK(CriWareErrorHandler_HandleMessage, "CriWareErrorHandler_HandleMessage at %p");
		ADD_HOOK(GGIregualDetector_ShowPopup, "GGIregualDetector_ShowPopup at %p");
		ADD_HOOK(DMMGameGuard_NPGameMonCallback, "DMMGameGuard_NPGameMonCallback at %p");
		// ADD_HOOK(DMMGameGuard_Setup, "DMMGameGuard_Setup at %p");
		ADD_HOOK(DMMGameGuard_SetCheckMode, "DMMGameGuard_SetCheckMode at %p");
		ADD_HOOK(set_fps, "set_fps at %p");
		ADD_HOOK(set_vsync_count, "set_vsync_count at %p");
		ADD_HOOK(Unity_Quit, "Unity_Quit at %p");

		LoadExtraAssetBundle();
		on_hotKey_0 = []() {
			startSCGUI();
			// needPrintStack = !needPrintStack;
		};
		SCCamera::initCameraSettings();
		g_on_hook_ready();
	}
}

void uninit_hook()
{
	if (!mh_inited)
		return;

	MH_DisableHook(MH_ALL_HOOKS);
	MH_Uninitialize();
}

bool init_hook()
{
	if (mh_inited)
		return false;

	if (MH_Initialize() != MH_OK)
		return false;

	g_on_close = []() {
		uninit_hook();
		TerminateProcess(GetCurrentProcess(), 0);
	};

	mh_inited = true;

	MH_CreateHook(LoadLibraryW, load_library_w_hook, &load_library_w_orig);
	MH_EnableHook(LoadLibraryW);
	return true;
}
