#include "files.h"
#include "common.h"
#include "errors.h"
#include "handles.h"
#include "strutil.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <dirent.h>
#include <mutex>
#include <optional>
#include <string>
#include <strings.h>
#include <map>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <unordered_map>
#include <utility>

kernel32::FsObject::~FsObject() {
	int fd = std::exchange(this->fd, -1);
	if (fd >= 0 && closeOnDestroy) {
		close(fd);
	}
	if (deletePending && !canonicalPath.empty()) {
		if (unlink(canonicalPath.c_str()) != 0) {
			perror("Failed to delete file on close");
		}
	}
}

namespace files {

static std::vector<std::string> splitList(const std::string &value, char delimiter) {
	std::vector<std::string> entries;
	size_t start = 0;
	while (start <= value.size()) {
		size_t end = value.find(delimiter, start);
		if (end == std::string::npos) {
			end = value.size();
		}
		entries.emplace_back(value.substr(start, end - start));
		if (end == value.size()) {
			break;
		}
		start = end + 1;
	}
	return entries;
}

static std::string toWindowsPathEntry(const std::string &entry) {
	if (entry.empty()) {
		return {};
	}
	bool looksWindows =
		entry.find('\\') != std::string::npos || (entry.size() >= 2 && entry[1] == ':' && entry[0] != '/');
	if (looksWindows) {
		std::string normalized = entry;
		std::replace(normalized.begin(), normalized.end(), '/', '\\');
		return normalized;
	}
	return pathToWindows(std::filesystem::path(entry));
}

static std::string toHostPathEntry(const std::string &entry) {
	if (entry.empty()) {
		return {};
	}
	auto converted = pathFromWindows(entry.c_str());
	if (!converted.empty()) {
		return converted.string();
	}
	std::string normalized = entry;
	std::replace(normalized.begin(), normalized.end(), '\\', '/');
	return normalized;
}

static HANDLE stdinHandle;
static HANDLE stdoutHandle;
static HANDLE stderrHandle;

static std::string getDriveMapping(char drive) {
	char envVar[] = "WIBO_DRIVE_X";
	envVar[11] = toupper(drive);
	const char *val = getenv(envVar);
	return val ? val : "";
}

struct PathMapEntry {
	std::string winPath;  // Normalized Windows-style prefix (forward slashes, no trailing slash)
	std::string hostPath; // Host filesystem path
};

// Parse WIBO_PATH_MAP once and cache the result.
// Format: "winPath=hostPath;winPath2=hostPath2;..."
static const std::vector<PathMapEntry> &getPathMap() {
	static std::vector<PathMapEntry> entries;
	static bool parsed = false;
	if (!parsed) {
		parsed = true;
		const char *envVal = getenv("WIBO_PATH_MAP");
		if (envVal) {
			std::string mapStr = envVal;
			size_t start = 0;
			while (start < mapStr.size()) {
				size_t end = mapStr.find(';', start);
				std::string entry = mapStr.substr(start, (end == std::string::npos) ? std::string::npos : end - start);
				size_t sep = entry.find('=');
				if (sep != std::string::npos) {
					std::string winPart = entry.substr(0, sep);
					std::string hostPart = entry.substr(sep + 1);
					std::replace(winPart.begin(), winPart.end(), '\\', '/');
					while (winPart.size() > 1 && winPart.back() == '/')
						winPart.pop_back();
					entries.push_back({std::move(winPart), std::move(hostPart)});
				}
				if (end == std::string::npos)
					break;
				start = end + 1;
			}
		}
	}
	return entries;
}

// --- /showIncludes path rewriting for ninja deps=msvc ---

// When WIBO_REWRITE_SHOWINCLUDES=1, rewrite "Note: including file:" lines
// on stdout using WIBO_PATH_MAP so ninja deps=msvc can track host paths.
static bool isShowIncludesRewriteEnabled() {
	static bool checked = false;
	static bool enabled = false;
	if (!checked) {
		checked = true;
		const char *env = getenv("WIBO_REWRITE_SHOWINCLUDES");
		enabled = env && std::string(env) == "1";
	}
	return enabled;
}

static constexpr const char *SHOW_INCLUDES_PREFIX = "Note: including file:";
static constexpr size_t SHOW_INCLUDES_PREFIX_LEN = 21; // strlen("Note: including file:")

// Resolve path case to match the actual filesystem (case-insensitive lookup).
// MSVC may report lowercase paths (e.g. src/xdk/libcmt/file.h) but the
// filesystem has different case (e.g. src/xdk/LIBCMT/file.h). On Linux,
// ninja can't stat the wrong-case path, so we fix it here.
static std::string normalizePathCase(const std::string &path) {
	if (path.empty() || path[0] != '/')
		return path;

	std::vector<std::string> components;
	size_t start = 1;
	while (start < path.size()) {
		size_t end = path.find('/', start);
		if (end == std::string::npos)
			end = path.size();
		components.push_back(path.substr(start, end - start));
		start = end + 1;
	}

	std::string resolved;
	for (const auto &comp : components) {
		std::string parent = resolved.empty() ? "/" : resolved;
		// First try exact match (fast path)
		std::string candidate = (parent == "/" ? "/" : parent + "/") + comp;
		struct stat st;
		if (stat(candidate.c_str(), &st) == 0) {
			resolved = candidate;
			continue;
		}
		// Case-insensitive search via readdir
		DIR *dir = opendir(parent.c_str());
		if (!dir) {
			// Can't open parent — just append remaining as-is
			resolved += "/" + comp;
			continue;
		}
		bool found = false;
		struct dirent *entry;
		while ((entry = readdir(dir)) != nullptr) {
			if (strcasecmp(entry->d_name, comp.c_str()) == 0) {
				resolved += "/";
				resolved += entry->d_name;
				found = true;
				break;
			}
		}
		closedir(dir);
		if (!found) {
			resolved += "/" + comp;
		}
	}
	return resolved;
}

// Rewrite a single "Note: including file:" line using the path map.
// Returns the rewritten line, or empty string if no rewrite needed.
static std::string rewriteShowIncludesLine(const char *line, size_t len) {
	// Strip trailing newline for processing
	size_t contentLen = len;
	while (contentLen > 0 && (line[contentLen - 1] == '\n' || line[contentLen - 1] == '\r'))
		contentLen--;

	if (contentLen <= SHOW_INCLUDES_PREFIX_LEN)
		return {};
	if (strncmp(line, SHOW_INCLUDES_PREFIX, SHOW_INCLUDES_PREFIX_LEN) != 0)
		return {};

	// Extract the indented path after the prefix
	const char *afterPrefix = line + SHOW_INCLUDES_PREFIX_LEN;
	size_t afterLen = contentLen - SHOW_INCLUDES_PREFIX_LEN;

	// Count leading spaces (indent)
	size_t indent = 0;
	while (indent < afterLen && afterPrefix[indent] == ' ')
		indent++;

	const char *pathStart = afterPrefix + indent;
	size_t pathLen = afterLen - indent;
	if (pathLen == 0)
		return {};

	std::string winPath(pathStart, pathLen);
	// Normalize to forward slashes
	std::replace(winPath.begin(), winPath.end(), '\\', '/');

	// Try path map entries (case-insensitive prefix match)
	const auto &pathMap = getPathMap();
	std::string winPathLower = winPath;
	std::transform(winPathLower.begin(), winPathLower.end(), winPathLower.begin(), ::tolower);

	for (const auto &entry : pathMap) {
		std::string entryLower = entry.winPath;
		std::transform(entryLower.begin(), entryLower.end(), entryLower.begin(), ::tolower);
		std::string prefix = entryLower + "/";
		if (winPathLower.substr(0, prefix.size()) == prefix) {
			std::string hostPath = entry.hostPath + "/" + winPath.substr(entry.winPath.size() + 1);
			hostPath = normalizePathCase(hostPath);
			std::string result = SHOW_INCLUDES_PREFIX;
			result.append(indent, ' ');
			result += hostPath;
			result += "\n";
			return result;
		}
	}

	// z: drive → root (wibo convention)
	if (winPath.size() >= 2 && (winPath[0] == 'z' || winPath[0] == 'Z') && winPath[1] == ':') {
		winPath = winPath.substr(2);
	}

	// Fix case mismatches (MSVC may report lowercase, filesystem may differ)
	if (!winPath.empty() && winPath[0] == '/') {
		winPath = normalizePathCase(winPath);
	}

	// Always return the normalized path (backslashes → forward slashes)
	std::string result = SHOW_INCLUDES_PREFIX;
	result.append(indent, ' ');
	result += winPath;
	result += "\n";
	return result;
}

// --- Cached filesystem helpers (gated by WIBO_FS_CACHE=1) ---

static bool isFsCacheEnabledFiles() {
	static bool checked = false;
	static bool enabled = false;
	if (!checked) {
		checked = true;
		const char *env = getenv("WIBO_FS_CACHE");
		enabled = env && std::string(env) == "1";
	}
	return enabled;
}

// Cached exists() — avoids repeated newfstatat syscalls for the same path
static std::unordered_map<std::string, bool> g_existsCache;
static unsigned g_existsCacheHits = 0;
static unsigned g_existsCacheMisses = 0;

static bool cachedExists(const std::filesystem::path &path) {
	if (isFsCacheEnabledFiles()) {
		std::string key = path.string();
		auto it = g_existsCache.find(key);
		if (it != g_existsCache.end()) {
			g_existsCacheHits++;
			return it->second;
		}
		g_existsCacheMisses++;
		std::error_code ec;
		bool result = std::filesystem::exists(path, ec);
		g_existsCache[key] = result;
		return result;
	}
	std::error_code ec;
	return std::filesystem::exists(path, ec);
}

// Cached directory listing — avoids repeated getdents64 syscalls
struct CachedDirEntryLocal {
	std::string name;
	std::filesystem::path fullPath;
};
static std::unordered_map<std::string, std::vector<CachedDirEntryLocal>> g_dirListCache;

static const std::vector<CachedDirEntryLocal> &cachedDirEntries(const std::filesystem::path &dir) {
	static const std::vector<CachedDirEntryLocal> empty;
	if (isFsCacheEnabledFiles()) {
		std::string key = dir.string();
		auto it = g_dirListCache.find(key);
		if (it != g_dirListCache.end()) {
			return it->second;
		}
		auto &entries = g_dirListCache[key];
		std::error_code ec;
		std::filesystem::directory_iterator iter{dir, ec};
		if (!ec) {
			for (const auto &entry : iter) {
				entries.push_back({entry.path().filename().string(), entry.path()});
			}
		}
		return entries;
	}
	return empty;
}

// Cached resolveCaseInsensitive — avoids per-component walks
static std::unordered_map<std::string, std::filesystem::path> g_caseInsensitiveCache;

static std::filesystem::path resolveCaseInsensitive(const std::filesystem::path &path) {
	std::filesystem::path norm = path.lexically_normal();

	// Check output cache first
	if (isFsCacheEnabledFiles()) {
		std::string key = norm.string();
		auto it = g_caseInsensitiveCache.find(key);
		if (it != g_caseInsensitiveCache.end()) {
			return it->second;
		}
	}

	if (cachedExists(norm)) {
		if (isFsCacheEnabledFiles()) {
			g_caseInsensitiveCache[norm.string()] = norm;
		}
		return norm;
	}

	std::filesystem::path newPath = ".";
	if (norm.is_absolute()) {
		newPath = norm.root_path();
	}

	bool followingExisting = true;
	auto it = norm.begin();
	if (norm.is_absolute()) {
		++it;
	}

	for (; it != norm.end(); ++it) {
		const auto &component = *it;
		std::filesystem::path nextPath = newPath / component;
		if (followingExisting && !cachedExists(nextPath) &&
			(component != ".." && component != "." && component != "")) {
			followingExisting = false;
			if (isFsCacheEnabledFiles()) {
				// Use cached directory listing
				const auto &entries = cachedDirEntries(newPath);
				for (const auto &entry : entries) {
					if (strcasecmp(entry.name.c_str(), component.string().c_str()) == 0) {
						followingExisting = true;
						nextPath = entry.fullPath;
						break;
					}
				}
			} else {
				std::error_code ec;
				std::filesystem::directory_iterator iter{newPath, ec};
				if (!ec) {
					for (std::filesystem::path entry : iter) {
						if (strcasecmp(entry.filename().c_str(), component.string().c_str()) == 0) {
							followingExisting = true;
							nextPath = entry;
							break;
						}
					}
				}
			}
		}
		newPath = nextPath;
	}

	std::filesystem::path result = followingExisting ? newPath : norm;
	if (followingExisting) {
		DEBUG_LOG("Resolved case-insensitive path: %s -> %s\n", path.c_str(), newPath.c_str());
	}
	if (isFsCacheEnabledFiles()) {
		g_caseInsensitiveCache[norm.string()] = result;
	}
	return result;
}

static std::filesystem::path applyPathMap(const std::string &inStr) {
	std::string str = inStr;
	std::replace(str.begin(), str.end(), '\\', '/');

	const auto &entries = getPathMap();
	if (entries.empty())
		return {};

	std::string strLower = str;
	toLowerInPlace(strLower);

	for (const auto &e : entries) {
		std::string winLower = e.winPath;
		toLowerInPlace(winLower);

		if (strLower.rfind(winLower, 0) == 0 &&
			(strLower.size() == winLower.size() || strLower[winLower.size()] == '/')) {
			std::string rest = str.substr(e.winPath.size());
			std::filesystem::path hostBase(e.hostPath);
			std::filesystem::path result;
			if (rest.empty() || (rest.size() == 1 && rest[0] == '/')) {
				result = hostBase;
			} else {
				if (rest[0] == '/')
					rest.erase(0, 1);
				result = hostBase / rest;
			}
			return resolveCaseInsensitive(result);
		}
	}
	DEBUG_LOG("applyPathMap: %s -> (no match)\n", str.c_str());
	return {};
}

static std::unordered_map<std::string, std::filesystem::path> g_pathFromWindowsCache;

std::filesystem::path pathFromWindows(const char *inStr) {
	// Check cache (gated by WIBO_FS_CACHE=1)
	static bool cacheEnabled = [] {
		const char *env = getenv("WIBO_FS_CACHE");
		return env && std::string(env) == "1";
	}();

	if (cacheEnabled) {
		auto it = g_pathFromWindowsCache.find(inStr);
		if (it != g_pathFromWindowsCache.end()) {
			return it->second;
		}
	}

	// Try path map first
	std::filesystem::path mapped = applyPathMap(inStr);
	if (!mapped.empty()) {
		if (cacheEnabled) g_pathFromWindowsCache[inStr] = mapped;
		return mapped;
	}

	// Normalize to forward slashes
	std::string str = inStr;
	std::replace(str.begin(), str.end(), '\\', '/');

	// Remove "//?/" prefix
	if (str.rfind("//?/", 0) == 0) {
		str.erase(0, 4);
	}

	// Handle drive letter mapping
	if (str.size() >= 2 && str[1] == ':') {
		std::string mapping = getDriveMapping(str[0]);
		if (!mapping.empty()) {
			std::string rest = (str.size() >= 3 && str[2] == '/') ? str.substr(3) : str.substr(2);
			std::filesystem::path p = std::filesystem::path(mapping) / rest;
			auto result = resolveCaseInsensitive(p);
			if (cacheEnabled) g_pathFromWindowsCache[inStr] = result;
			return result;
		}
		// Fallback: strip drive letter
		if (str.size() >= 3 && str[2] == '/') {
			str.erase(0, 2);
		}
	}

	auto result = resolveCaseInsensitive(str);
	if (cacheEnabled) g_pathFromWindowsCache[inStr] = result;
	return result;
}

std::string pathToWindows(const std::filesystem::path &path) {
	std::string hostStr = std::filesystem::absolute(path).lexically_normal().string();

	// Try path map first (most specific first)
	const auto &entries = getPathMap();
	for (const auto &e : entries) {
		std::string hostPart = std::filesystem::absolute(e.hostPath).lexically_normal().string();
		if (hostStr.rfind(hostPart, 0) == 0) {
			std::string rest = hostStr.substr(hostPart.size());
			if (!rest.empty() && (rest[0] == '/' || rest[0] == '\\'))
				rest.erase(0, 1);
			std::string result = e.winPath;
			if (!result.empty() && result.back() != '\\' && result.back() != '/' && !rest.empty())
				result += '\\';
			result += rest;
			std::replace(result.begin(), result.end(), '/', '\\');
			return result;
		}
	}

	std::string str = path.lexically_normal().string();

	// Check for mapped drives
	for (char d = 'A'; d <= 'Z'; ++d) {
		std::string mapping = getDriveMapping(d);
		if (mapping.empty())
			continue;
		std::string mappingNorm = std::filesystem::path(mapping).lexically_normal().string();
		if (str.rfind(mappingNorm, 0) == 0) {
			std::string drivePrefix = "X:";
			drivePrefix[0] = d;
			str.replace(0, mappingNorm.size(), drivePrefix);
			std::replace(str.begin(), str.end(), '/', '\\');
			return str;
		}
	}

	if (path.is_absolute()) {
		str.insert(0, "Z:");
	}

	std::replace(str.begin(), str.end(), '/', '\\');
	return str;
}

IOResult read(FileObject *file, void *buffer, size_t bytesToRead, const std::optional<off_t> &offset,
			  bool updateFilePointer) {
	IOResult result{};
	if (!file || !file->valid()) {
		result.unixError = EBADF;
		return result;
	}
	if (bytesToRead == 0) {
		return result;
	}

	// Sanity check: if no offset is given, we must update the file pointer
	assert(offset.has_value() || updateFilePointer);

	if (file->isPipe) {
		std::lock_guard lk(file->m);
		size_t chunk = bytesToRead > SSIZE_MAX ? SSIZE_MAX : bytesToRead;
		uint8_t *in = static_cast<uint8_t *>(buffer);
		ssize_t rc;
		while (true) {
			rc = ::read(file->fd, in, chunk);
			if (rc == -1 && errno == EINTR) {
				continue;
			}
			break;
		}
		if (rc == -1) {
			result.unixError = errno ? errno : EIO;
			return result;
		}
		if (rc == 0) {
			result.reachedEnd = true;
			return result;
		}
		result.bytesTransferred = static_cast<size_t>(rc);
		return result;
	}

	const auto doRead = [&](off_t pos) {
		size_t total = 0;
		size_t remaining = bytesToRead;
		uint8_t *in = static_cast<uint8_t *>(buffer);
		while (remaining > 0) {
			size_t chunk = remaining > SSIZE_MAX ? SSIZE_MAX : remaining;
			ssize_t rc = pread(file->fd, in + total, chunk, pos);
			if (rc == -1) {
				if (errno == EINTR) {
					continue;
				}
				result.unixError = errno ? errno : EIO;
				break;
			}
			if (rc == 0) {
				result.reachedEnd = true;
				break;
			}
			total += static_cast<size_t>(rc);
			remaining -= static_cast<size_t>(rc);
			pos += rc;
		}
		result.bytesTransferred = total;
	};

	if (updateFilePointer || !offset.has_value()) {
		std::lock_guard lk(file->m);
		const off_t pos = offset.value_or(file->filePos);
		doRead(pos);
		if (updateFilePointer) {
			file->filePos = pos + static_cast<off_t>(result.bytesTransferred);
		}
	} else {
		doRead(*offset);
	}

	return result;
}

IOResult write(FileObject *file, const void *buffer, size_t bytesToWrite, const std::optional<off_t> &offset,
			   bool updateFilePointer) {
	IOResult result{};
	if (!file || !file->valid()) {
		result.unixError = EBADF;
		return result;
	}
	if (bytesToWrite == 0) {
		return result;
	}

	// Sanity check: if no offset is given, we must update the file pointer
	assert(offset.has_value() || updateFilePointer);

	// Rewrite /showIncludes paths on stdout for ninja deps=msvc tracking
	if (file->fd == STDOUT_FILENO && isShowIncludesRewriteEnabled()) {
		std::lock_guard lk(file->m);
		const char *data = static_cast<const char *>(buffer);
		size_t pos = 0;
		size_t totalWritten = 0;
		while (pos < bytesToWrite) {
			// Find end of line
			const char *nl = static_cast<const char *>(memchr(data + pos, '\n', bytesToWrite - pos));
			size_t lineEnd = nl ? (nl - data + 1) : bytesToWrite;
			size_t lineLen = lineEnd - pos;

			std::string rewritten = rewriteShowIncludesLine(data + pos, lineLen);
			if (!rewritten.empty()) {
				ssize_t rc = ::write(STDOUT_FILENO, rewritten.data(), rewritten.size());
				if (rc > 0) totalWritten += lineLen; // count original bytes as transferred
			} else {
				ssize_t rc = ::write(STDOUT_FILENO, data + pos, lineLen);
				if (rc > 0) totalWritten += static_cast<size_t>(rc);
			}
			pos = lineEnd;
		}
		result.bytesTransferred = totalWritten;
		return result;
	}

	if (file->appendOnly || file->isPipe) {
		std::lock_guard lk(file->m);
		size_t total = 0;
		size_t remaining = bytesToWrite;
		const uint8_t *in = static_cast<const uint8_t *>(buffer);
		while (remaining > 0) {
			size_t chunk = remaining > SSIZE_MAX ? SSIZE_MAX : remaining;
			ssize_t rc = ::write(file->fd, in + total, chunk);
			if (rc == -1) {
				if (errno == EINTR) {
					continue;
				}
				result.unixError = errno ? errno : EIO;
				break;
			}
			if (rc == 0) {
				break;
			}
			total += static_cast<size_t>(rc);
			remaining -= static_cast<size_t>(rc);
		}
		result.bytesTransferred = total;
		if (updateFilePointer) {
			off_t pos = file->isPipe ? 0 : lseek(file->fd, 0, SEEK_CUR);
			if (pos >= 0) {
				file->filePos = pos;
			} else if (result.unixError == 0) {
				result.unixError = errno ? errno : EIO;
			}
		}
		return result;
	}

	auto doWrite = [&](off_t pos) {
		size_t total = 0;
		size_t remaining = bytesToWrite;
		const uint8_t *in = static_cast<const uint8_t *>(buffer);
		while (remaining > 0) {
			size_t chunk = remaining > SSIZE_MAX ? SSIZE_MAX : remaining;
			ssize_t rc = pwrite(file->fd, in + total, chunk, pos);
			if (rc == -1) {
				if (errno == EINTR) {
					continue;
				}
				result.unixError = errno ? errno : EIO;
				break;
			}
			if (rc == 0) {
				break;
			}
			total += static_cast<size_t>(rc);
			remaining -= static_cast<size_t>(rc);
			pos += rc;
		}
		result.bytesTransferred = total;
	};

	if (updateFilePointer || !offset.has_value()) {
		std::lock_guard lk(file->m);
		const off_t pos = offset.value_or(file->filePos);
		doWrite(pos);
		if (updateFilePointer) {
			file->filePos = pos + static_cast<off_t>(result.bytesTransferred);
		}
	} else {
		doWrite(*offset);
	}

	return result;
}

HANDLE getStdHandle(DWORD nStdHandle) {
	switch (nStdHandle) {
	case STD_INPUT_HANDLE:
		return stdinHandle;
	case STD_OUTPUT_HANDLE:
		return stdoutHandle;
	case STD_ERROR_HANDLE:
		return stderrHandle;
	default:
		return INVALID_HANDLE_VALUE;
	}
}

BOOL setStdHandle(DWORD nStdHandle, HANDLE hHandle) {
	switch (nStdHandle) {
	case STD_INPUT_HANDLE:
		stdinHandle = hHandle;
		break;
	case STD_OUTPUT_HANDLE:
		stdoutHandle = hHandle;
		break;
	case STD_ERROR_HANDLE:
		stderrHandle = hHandle;
		break;
	default:
		return 0; // fail
	}
	return 1; // success
}

void init() {
	signal(SIGPIPE, SIG_IGN);
	auto &handles = wibo::handles();
	auto stdinObject = make_pin<FileObject>(STDIN_FILENO);
	stdinObject->closeOnDestroy = false;
	stdinHandle = handles.alloc(std::move(stdinObject), FILE_GENERIC_READ, 0);
	auto stdoutObject = make_pin<FileObject>(STDOUT_FILENO);
	stdoutObject->closeOnDestroy = false;
	stdoutObject->appendOnly = true;
	stdoutHandle = handles.alloc(std::move(stdoutObject), FILE_GENERIC_WRITE, 0);
	auto stderrObject = make_pin<FileObject>(STDERR_FILENO);
	stderrObject->closeOnDestroy = false;
	stderrObject->appendOnly = true;
	stderrHandle = handles.alloc(std::move(stderrObject), FILE_GENERIC_WRITE, 0);
}

// Cache for findCaseInsensitiveFile: (dir+"/"+lowercase_name) -> result
static std::unordered_map<std::string, std::optional<std::filesystem::path>> g_findFileCache;

std::optional<std::filesystem::path> findCaseInsensitiveFile(const std::filesystem::path &directory,
															 const std::string &filename) {
	if (directory.empty() || filename.empty()) {
		return std::nullopt;
	}

	// Check cache
	std::string cacheKey;
	if (isFsCacheEnabledFiles()) {
		std::string lowerName = filename;
		toLowerInPlace(lowerName);
		cacheKey = directory.string() + "/" + lowerName;
		auto it = g_findFileCache.find(cacheKey);
		if (it != g_findFileCache.end()) {
			return it->second;
		}
	}

	std::error_code ec;
	if (!cachedExists(directory)) {
		if (isFsCacheEnabledFiles()) g_findFileCache[cacheKey] = std::nullopt;
		return std::nullopt;
	}

	std::string needle = filename;
	toLowerInPlace(needle);

	if (isFsCacheEnabledFiles()) {
		// Use cached directory listing
		const auto &entries = cachedDirEntries(directory);
		for (const auto &entry : entries) {
			std::string candidate = entry.name;
			toLowerInPlace(candidate);
			if (candidate == needle) {
				auto result = canonicalPath(entry.fullPath);
				g_findFileCache[cacheKey] = result;
				return result;
			}
		}
	} else {
		for (const auto &entry : std::filesystem::directory_iterator(directory, ec)) {
			if (ec) break;
			std::string candidate = entry.path().filename().string();
			toLowerInPlace(candidate);
			if (candidate == needle) {
				return canonicalPath(entry.path());
			}
		}
	}

	auto direct = directory / filename;
	if (cachedExists(direct)) {
		auto result = canonicalPath(direct);
		if (isFsCacheEnabledFiles()) g_findFileCache[cacheKey] = result;
		return result;
	}
	if (isFsCacheEnabledFiles()) g_findFileCache[cacheKey] = std::nullopt;
	return std::nullopt;
}

static std::unordered_map<std::string, std::filesystem::path> g_canonicalCache;

std::filesystem::path canonicalPath(const std::filesystem::path &path) {
	// Check env-gated cache
	static bool cacheEnabled = [] {
		const char *env = getenv("WIBO_FS_CACHE");
		return env && std::string(env) == "1";
	}();

	if (cacheEnabled) {
		std::string key = path.string();
		auto it = g_canonicalCache.find(key);
		if (it != g_canonicalCache.end()) {
			return it->second;
		}
		std::error_code ec;
		auto canonical = std::filesystem::weakly_canonical(path, ec);
		std::filesystem::path result = !ec ? canonical : std::filesystem::absolute(path);
		g_canonicalCache[key] = result;
		return result;
	}

	std::error_code ec;
	auto canonical = std::filesystem::weakly_canonical(path, ec);
	if (!ec) {
		return canonical;
	}
	return std::filesystem::absolute(path);
}

std::string hostPathListToWindows(const std::string &value) {
	if (value.empty()) {
		return value;
	}
	char delimiter = value.find(';') != std::string::npos ? ';' : ':';
	auto entries = splitList(value, delimiter);
	std::string result;
	for (size_t i = 0; i < entries.size(); ++i) {
		if (i != 0) {
			result.push_back(';');
		}
		if (!entries[i].empty()) {
			result += toWindowsPathEntry(entries[i]);
		}
	}
	return result;
}

std::string windowsPathListToHost(const std::string &value) {
	if (value.empty()) {
		return value;
	}
	auto entries = splitList(value, ';');
	std::string result;
	for (size_t i = 0; i < entries.size(); ++i) {
		if (i != 0) {
			result.push_back(':');
		}
		if (!entries[i].empty()) {
			result += toHostPathEntry(entries[i]);
		}
	}
	return result;
}
static std::mutex gMappedFileMutex;
static std::map<std::pair<dev_t, ino_t>, int> gMappedFileCount;

void trackMappedFile(dev_t dev, ino_t ino) {
	std::lock_guard lk(gMappedFileMutex);
	int count = ++gMappedFileCount[{dev, ino}];
	DEBUG_LOG("trackMappedFile: dev=%lu ino=%lu count=%d\n",
		(unsigned long)dev, (unsigned long)ino, count);
}

void untrackMappedFile(dev_t dev, ino_t ino) {
	std::lock_guard lk(gMappedFileMutex);
	auto it = gMappedFileCount.find({dev, ino});
	if (it != gMappedFileCount.end()) {
		if (--it->second <= 0) {
			DEBUG_LOG("untrackMappedFile: dev=%lu ino=%lu (removed)\n",
				(unsigned long)dev, (unsigned long)ino);
			gMappedFileCount.erase(it);
		} else {
			DEBUG_LOG("untrackMappedFile: dev=%lu ino=%lu count=%d\n",
				(unsigned long)dev, (unsigned long)ino, it->second);
		}
	}
}

bool isFileMapped(int fd) {
	struct stat st {};
	if (fstat(fd, &st) != 0) {
		DEBUG_LOG("isFileMapped: fstat failed for fd=%d\n", fd);
		return false;
	}
	std::lock_guard lk(gMappedFileMutex);
	bool mapped = gMappedFileCount.count({st.st_dev, st.st_ino}) > 0;
	DEBUG_LOG("isFileMapped: fd=%d dev=%lu ino=%lu -> %s\n",
		fd, (unsigned long)st.st_dev, (unsigned long)st.st_ino,
		mapped ? "YES (skip truncation)" : "no");
	return mapped;
}

void reportFilesCacheStats() {
	if (isFsCacheEnabledFiles() && getenv("WIBO_FS_CACHE_STATS")) {
		fprintf(stderr, "[wibo] files cache: exists %u/%u, caseInsensitive %zu, findFile %zu, dirList %zu, canonical %zu (hits/misses or entries)\n",
			g_existsCacheHits, g_existsCacheMisses,
			g_caseInsensitiveCache.size(),
			g_findFileCache.size(),
			g_dirListCache.size(),
			g_canonicalCache.size());
	}
}

} // namespace files
