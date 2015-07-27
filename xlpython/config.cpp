#include "xlpython.h"

#undef GetEnvironmentStrings // the windows headers screws this one up

Config::ConfigMap Config::configs;

void SplitPath(const std::string& path, Config::ValueMap& values, const std::string& prefix)
{
	values[prefix + "Path"] = path;
	
	std::string filename;
	size_t itSlash = path.rfind("\\");
	if(itSlash == std::string::npos)
	{
		filename = path;
	}
	else
	{
		filename = path.substr(itSlash+1, path.length() - itSlash);
		values[prefix + "Dir"] = path.substr(0, itSlash);
	}
	values[prefix + "FileName"] = filename;
	
	size_t itDot = filename.rfind(".");
	if(itDot != std::string::npos)
	{
		values[prefix + "Ext"] = filename.substr(itDot+1, path.length() - itDot);
		values[prefix + "Name"] = filename.substr(0, itDot);
	}
	else
	{
		values[prefix + "Ext"] = "";
		values[prefix + "Name"] = filename;
	}
}

void AddEnvironmentVariables(Config::ValueMap& values)
{
	const char* pEnvStrs = GetEnvironmentStrings();
	while(*pEnvStrs != 0)
	{
		std::string keyEqualsValue = pEnvStrs;
		
		size_t equalsPos = keyEqualsValue.find("=");
		if(equalsPos == std::string::npos)
			throw formatted_exception() << "Could not parse string returned by GetEnvironmentStrings: " << keyEqualsValue;
		
		std::string key = keyEqualsValue.substr(0, equalsPos);
		std::string value = keyEqualsValue.substr(equalsPos + 1, keyEqualsValue.length() - equalsPos - 1);
		
		std::transform(key.begin(), key.end(), key.begin(), std::toupper);
		
		values["Environment:" + key] = value;
		
		pEnvStrs += keyEqualsValue.length() + 1;
	}
}

#define MAX_KEY_LENGTH 255
#define MAX_VALUE_NAME 16383

void AddPythonRegistryInfo(Config::ValueMap& values)
{
	AutoCloseHKey hKeyPythonCore(NULL);
	HKEY hRootKeys[] = {
		HKEY_LOCAL_MACHINE,
		HKEY_CURRENT_USER,
	};

	for (int iRootKey = 0; iRootKey < 2; iRootKey++)
	{
		DWORD nSubKeys = 0;
		DWORD hRes = 0;
		int pyMinor[] = { -1, -1, -1, -1 };

		hRes = ::RegOpenKeyExA(
			hRootKeys[iRootKey],
			"Software\\Python\\PythonCore",
			0,
			KEY_READ,
			&(hKeyPythonCore.hKey)
		);

		if (ERROR_SUCCESS == hRes)
		{
			hRes = ::RegQueryInfoKeyA(
				hKeyPythonCore,
				NULL, 0,
				NULL,
				&nSubKeys,
				NULL,
				NULL,
				NULL,
				NULL,
				NULL,
				NULL,
				NULL
			);

			if (ERROR_SUCCESS != hRes)
				continue;

			for (DWORD iSubKey = 0; iSubKey < nSubKeys; iSubKey++)
			{
				char pName[255];
				DWORD nBytesName = 255;

				hRes = ::RegEnumKeyExA(
					hKeyPythonCore,
					iSubKey,
					pName,
					&nBytesName,
					NULL,
					NULL,
					NULL,
					NULL
				);

				if (ERROR_SUCCESS != hRes)
					continue;

				if (strlen(pName) >= 3 && (pName[0] == '2' || pName[0] == '3') && pName[1] == '.')
				{
					int major = pName[0] == '2' ? 2 : 3;
					int minor = atoi(pName + 2);

					if (minor == 0)
						continue;

					strcat_s(pName, 255, "\\InstallPath");

					char pFolderPath[MAX_PATH];
					LONG nBytesFolderPath = MAX_PATH;

					hRes = ::RegQueryValueA(
						hKeyPythonCore,
						pName,
						pFolderPath,
						&nBytesFolderPath
					);

					if (ERROR_SUCCESS != hRes)
						continue;

					char pConfigName[255];
					if (-1 != sprintf_s(pConfigName, 255, "Registry:Python%i%iFolder", major, minor))
					{
						values[pConfigName] = pFolderPath;
					}

					if (minor > pyMinor[major])
					{
						if (-1 != sprintf_s(pConfigName, 255, "Registry:Python%iFolder", major))
						{
							values[pConfigName] = pFolderPath;
						}
						pyMinor[major] = minor;
					}
				}
			}
		}
	}
}

Config::Config(const std::string& filename)
	: pInterface(NULL)
	, hJob(NULL)
{
	GetLastWriteTime(filename.c_str(), &ftLastModify);

	SplitPath(filename, values, "Config");
	SplitPath(GetDLLPath(), values, "Dll");
	std::string configDir;
	if(TryGetValue("ConfigDir", configDir))
	{
		std::string workbookDir = configDir + "\\..";
		char workbookDirOut[MAX_PATH];
		if(0 == GetFullPathNameA(workbookDir.c_str(), MAX_PATH, workbookDirOut, NULL))
			throw formatted_exception() << "GetFullPathNameA failed: " << GetLastErrorMessage() << "\n" << "Argument: " << workbookDir;
		values["WorkbookDir"] = workbookDirOut;
	}
	AddEnvironmentVariables(values);
	AddPythonRegistryInfo(values);

	GUID guid;
	NewGUID(guid);
	values["RandomGUID"] = GUIDToStdString(guid);

	std::ifstream f(filename.c_str());

	if(!f.is_open())
		throw formatted_exception() << "Could not open config file '" << filename << "'.";
	std::string line;
	while(true)
	{
		if(f.eof())
			break;

		std::getline(f, line);
		strtrim(line);
		if(line.empty() || line[0] == '#')
			continue;
		size_t itEquals = line.find('=');
		if(itEquals == std::string::npos)
			throw formatted_exception() << "Error in config file, lines must either be empty, comments starting with '#' or of the form key=value";

		std::string key = strtrim(line.substr(0, itEquals));
		std::string value = strtrim(line.substr(itEquals + 1, line.length() - itEquals - 1));
		
		while(true)
		{
			size_t itMacroStart = value.find("$(");
			if(itMacroStart == std::string::npos)
				break;
			size_t itMacroEnd = value.find(")", itMacroStart);
			if(itMacroStart == std::string::npos)
				throw formatted_exception() << "Macro $(...) was not closed in configuration file: " << line << ".";
			
			std::string macro = value.substr(itMacroStart + 2, itMacroEnd - itMacroStart - 2);
			std::string macroValue;
			bool macroTrailSlash = false;
			if (macro.length() > 0 && macro[macro.length() - 1] == '\\')
			{
				macroTrailSlash = true;
				macro = macro.substr(0, macro.length() - 1);
			}
			if(macro.length() > 0 && macro[0] == '?')
				macroValue = GetValue(macro.substr(1), "");
			else
				macroValue = GetValue(macro);
			if (macroTrailSlash)
			{
				if (macroValue.length() > 0 && macroValue[macroValue.length() - 1] != '\\')
					macroValue += "\\";
			}
			value = value.replace(itMacroStart, itMacroEnd - itMacroStart + 1, macroValue);
		}

		values[key] = value;
	}
}

Config::~Config()
{
	// release dispatch object if present
	if(pInterface != NULL)
		pInterface->Release();

	// release the job handle if present
	if(hJob != NULL)
		CloseHandle(hJob);
}

Config* Config::GetConfig(const std::string& filename)
{
	std::string fullpath;
	GetFullPathRelativeToDLLFolder(filename, fullpath);

	Config::ConfigMap::iterator it = configs.find(fullpath);
	if(it == configs.end())
	{
		Config* c;
		configs[fullpath] = c = new Config(fullpath);
		return c;
	}
	else
	{
		// check if config file has been updated in the meantime
		FILETIME ftLastModify;
		GetLastWriteTime(fullpath.c_str(), &ftLastModify);
		if(-1 == CompareFileTime(&it->second->ftLastModify, &ftLastModify))
		{
			delete it->second;
			configs.erase(it);

			Config* c;
			configs[fullpath] = c = new Config(fullpath);
			return c;
		}

		return it->second;
	}
}

int Config::GetValueAsInt(const std::string& key)
{
	Config::ValueMap::iterator it = values.find(key);
	
	if(it == values.end())
		throw formatted_exception() << "Key '" << key << "' not found in configuration (nor is it pre-defined).";

	char* end;
	long int value = strtol(it->second.c_str(), &end, 0);
	if(*end == 0)
		return value;

	throw formatted_exception() << "Could not convert '" << key << " = " << it->second << "' to integer.";
}

int Config::GetValueAsInt(const std::string& key, int dfault)
{
	Config::ValueMap::iterator it = values.find(key);
	
	if(it == values.end())
		return dfault;

	char* end;
	long int value = strtol(it->second.c_str(), &end, 0);
	if(*end == 0)
		return value;

	throw formatted_exception() << "Could not convert '" << key << " = " << it->second << "' to integer.";
}

std::string Config::GetValue(const std::string& key)
{
	Config::ValueMap::iterator it = values.find(key);
	
	if(it == values.end())
		throw formatted_exception() << "Key '" << key << "' not found in configuration (nor is it pre-defined).";

	return it->second;
}

std::string Config::GetValue(const std::string& key, const std::string& dfault)
{
	Config::ValueMap::iterator it = values.find(key);
	
	if(it == values.end())
		return dfault;

	return it->second;
}

bool Config::HasValue(const std::string& key)
{
	Config::ValueMap::iterator it = values.find(key);
	
	return it != values.end();
}

bool Config::TryGetValue(const std::string& key, std::string& value)
{
	Config::ValueMap::iterator it = values.find(key);
	if(it == values.end())
		return false;
	else
	{
		value = it->second;
		return true;
	}
}


Config::ValueMap::iterator Config::GetIterator()
{
	return values.begin();
}


bool Config::CheckRPCServer()
{
	UINT nTypeInfo;
	HRESULT hr = pInterface->GetTypeInfoCount(&nTypeInfo);
	return 0x800706ba != hr;		// RPC server not available = HRESULT 0x800706ba
}

void Config::ActivateRPCServer()
{
	HRESULT hr;

	// release existing object if present
	if(pInterface != NULL)
	{
		pInterface->Release();
		pInterface = NULL;
	}

	// release existing job if present
	if(hJob != NULL)
	{
		CloseHandle(hJob);
		hJob = NULL;
	}
	AutoCloseHandle hJobAuto(NULL);

	// get guid
	GUID clsid;
	ParseGUID(this->GetValue("CLSID").c_str(), clsid);

	// try to create an instance of the Python interface object
	hr = CoCreateInstance(clsid, NULL, CLSCTX_LOCAL_SERVER, IID_IDispatch, (void**) &(this->pInterface));

	// if the server's not running, try to start it up
	if(hr == REGDB_E_CLASSNOTREG)
	{
		// build the command line with which to start the Python process
		std::string workingDir = this->GetValue("WorkingDir");
		std::string pythonCmd = this->GetValue("Command");
		char cmdLine[2048] = "";
		strcat_s(cmdLine, pythonCmd.c_str());
		
		// build environment variables
		std::unordered_map<std::string, std::string> environment;
		if(this->HasValue("EnvironmentInclude"))
		{
			std::vector<std::string> v;
			strsplit(this->GetValue("EnvironmentInclude"), ",", v);
			std::string prefix = "Environment:";
			for(size_t k=0; k<v.size(); k++)
			{
				strupper(v[k]);
				std::string value;
				if(this->TryGetValue(prefix + v[k], value))
					environment[v[k]] = value;
			}
		}
		else
		{
			std::unordered_set<std::string> exclude;
			if(this->HasValue("EnvironmentExclude"))
			{
				std::vector<std::string> v;
				strsplit(this->GetValue("EnvironmentExclude"), ",", v);
				for(size_t k=0; k<v.size(); k++)
				{
					strupper(v[k]);
					exclude.insert(v[k]);
				}
			}
			std::string prefix = "Environment:";
			for(Config::ValueMap::iterator it = this->values.begin(); it != this->values.end(); it++)
			{
				if(it->first.length() > prefix.length() && it->first.substr(0, prefix.length()) == prefix)
				{
					std::string key = it->first.substr(prefix.length());
					std::unordered_set<std::string>::iterator it2 = exclude.find(key);
					if(it2 == exclude.end())
						environment[key] = it->second;
				}
			}
		}
		size_t nChars = 0;
		for(std::unordered_map<std::string, std::string>::iterator it = environment.begin(); it != environment.end(); it++)
			nChars += it->first.length() + 1 + it->second.length() + 1;
		AutoArrayDeleter<char> envStr(new char[nChars+1]);
		char* pEnvStr = envStr.p;
		for(std::unordered_map<std::string, std::string>::iterator it = environment.begin(); it != environment.end(); it++)
		{
			memcpy(pEnvStr, it->first.c_str(), it->first.length());
			pEnvStr += it->first.length();
			*pEnvStr = '=';
			pEnvStr++;
			memcpy(pEnvStr, it->second.c_str(), it->second.length());
			pEnvStr += it->second.length();
			*pEnvStr = 0;
			pEnvStr++;
		}
		*pEnvStr = 0;

		// initialize structures for CreateProcess
		STARTUPINFOA si;
		ZeroMemory(&si, sizeof(si));
		si.cb = sizeof(si);
		PROCESS_INFORMATION pi;
		ZeroMemory(&pi, sizeof(pi));

		// create a file to which to redirect stdout and stderr, if specified
		AutoCloseHandle hStdOut(NULL);
		if(this->HasValue("RedirectOutput"))
		{
			std::string filename = this->GetValue("RedirectOutput");
			SECURITY_ATTRIBUTES sa;
			ZeroMemory(&sa, sizeof(sa));
			sa.nLength = sizeof(sa);
			sa.bInheritHandle = true;

			hStdOut.handle = CreateFileA(
				filename.c_str(),
				GENERIC_WRITE,
				FILE_SHARE_READ,
				&sa,
				CREATE_ALWAYS,
				FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH ,
				NULL);

			if(INVALID_HANDLE_VALUE == hStdOut.handle)
				throw formatted_exception() << "Could not open '" << filename << "' for output redirection.";

			si.dwFlags |= STARTF_USESTDHANDLES;
			si.hStdError = hStdOut;
			si.hStdOutput = hStdOut;
		}

		// create the job
		if(this->GetValueAsInt("KillWithHostProcess", 1))
		{
			hJobAuto.handle = CreateJobObject(NULL, NULL);
			if(NULL == hJobAuto.handle)
				throw formatted_exception() << "CreateJobObject failed:" << GetLastErrorMessage();
			
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION jxli;
			ZeroMemory(&jxli, sizeof(jxli));
			jxli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			if(!SetInformationJobObject(hJobAuto.handle, JobObjectExtendedLimitInformation, &jxli, sizeof(jxli)))
				throw formatted_exception() << "SetInformationJobObject failed: " << GetLastErrorMessage();
		}

		// create the Python process
		if(!CreateProcessA(
			NULL, 
			cmdLine, 
			NULL, 
			NULL, 
			TRUE, 
			NORMAL_PRIORITY_CLASS | CREATE_BREAKAWAY_FROM_JOB, 
			envStr.p, 
			workingDir.c_str(), 
			&si, 
			&pi))
		{
			formatted_exception e;
			e << "Could not create Python process.\n";
			e << "Error message: " << GetLastErrorMessage() << "\n";
			e << "Command: " << cmdLine << "\nWorking Dir: " << workingDir;
			throw e;
		}
		AutoCloseHandle hPiThread(pi.hThread);
		AutoCloseHandle hPiProcess(pi.hProcess);

		// add the process to the job
		if(hJobAuto.handle != NULL)
		{
			if(!AssignProcessToJobObject(hJobAuto.handle, pi.hProcess))
				throw formatted_exception() << "AssignProcessToJobObject failed: " << GetLastErrorMessage();
		}

		// now repeatedly try to create the Python interface object, waiting up to 1 minute to do it
		for(int k=0; k<600; k++)
		{
			// try to create the object
			hr = CoCreateInstance(clsid, NULL, CLSCTX_LOCAL_SERVER, IID_IDispatch, (void**) &(this->pInterface));
			if(hr != REGDB_E_CLASSNOTREG)
				break;

			// didn't create object - check that python process is still there!
			DWORD dwExitCode;
			if(0 == GetExitCodeProcess(pi.hProcess, &dwExitCode))
				throw formatted_exception() << "GetExitCodeProcess failed: " << GetLastErrorMessage();
			if(dwExitCode != STILL_ACTIVE)
			{
				formatted_exception e;
				e << "Python process exited before it was possible to create the interface object.";
				if(this->HasValue("RedirectOutput"))
					e << " Try consulting '" << this->GetValue("RedirectOutput") << "'.";
				e << "\n";
				e << "Command: " << cmdLine << "\nWorking Dir: " << workingDir;
				throw e;
			}

			Sleep(100);
		}
	}

	// if still haven't managed to get the object throw an error
	if(FAILED(hr))
		throw formatted_exception() << "Could not activate Python COM server, hr = " << hr;

	// wrap the object
	pInterface = new CDispatchWrapper(pInterface);

	// give up job handle to config class
	this->hJob = hJobAuto.handle;
	hJobAuto.handle = NULL;
}