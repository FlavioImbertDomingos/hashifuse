﻿/****************************************************************************
**
** tfefs - FUSE client for browsing Hashicorp Terraform Enterprise.
**
** Authored by John Boero
** Build instructions: g++ -D_FILE_OFFSET_BITS=64 -lfuse -lcurl -ljsoncpp main.cpp
** Usage: ./tfefs -s -o direct_io /path/to/mount
**
** Note single threaded mode is currently mandatory for tfefs (-s flag)
** Note direct_io is mandatory right now until we can get key size in getattrs.
** Environment Variables: 
	TFE_ADDR		tfe address, or app.terraform.io by default (SaaS)  Example: "http://localhost:8200"
	TFE_TOKEN		bearer token.

** This code is kept fairly simple/ugly without object oriented best practices.
** TODO: securely destroy strings - https://stackoverflow.com/questions/5698002/how-does-one-securely-clear-stdstring
** TODO: writes are not guaranteed currently.
****************************************************************************/

#define CURL_STATICLIB
#define FUSE_USE_VERSION 26

#include <string>
#include <string.h>
#include <sstream>
#include <vector>
#include <iostream>
#include <algorithm>
#include <regex>
#include <libgen.h>

#include <curl/curl.h>
#include <json/json.h>

#include <mutex>
#include <fstream>
#include <unistd.h>
#include <sys/xattr.h>
#include <stdarg.h>
#include <fuse.h>

// Term colors for stdout
const char RESET[]	= "\033[0m";
const char RED[]	= "\033[1;31m";
const char YELLOW[]	= "\e[0;33m";
const char CYAN[]	= "\e[1;36m";
const char PURPLE[]	= "\e[0;35m";
const char GREEN[]	= "\e[0;32m";
const char BLUE[]	= "\e[0;34m";

using namespace std;

// Global api version.
const string apiVers = "/api/v2";

// Set logs to other options (ofstream) or default to std::cout
ostream *logs = &cout;

// Protect multi-threaded mode from libcurl/libopenssl race condition.
mutex curlmutex;

// CURL callback
namespace
{
    std::size_t callback(
            const char* in,
            std::size_t size,
            std::size_t num,
            char* out)
    {
        string data(in, (size_t) size * num);
        *((stringstream*) out) << data;

        #if DEBUG
		*logs << GREEN << data << RESET << endl;
		#endif
        return size * num;
    }
}

// tfefs GET raw via libcurl
// Currently supports request GET (default), POST, LIST.
// TODO: escape environment variables for injection vulnerabilities.
int	tfeCURL(string url, stringstream &httpData, string request = "GET", const string post = "")
{
	int res = 0, httpCode = 0;
	const char *nsHeader = "Content-Type: application/vnd.api+json";
	string tokenHeader = "Authorization: Bearer ";
	struct curl_slist *headers = curl_slist_append(NULL, (tokenHeader + getenv("TFE_TOKEN")).c_str());
	headers = curl_slist_append(headers, nsHeader);
	CURL* curl;

	if (getenv("TFE_ADDR"))
		url = (string)getenv("TFE_ADDR") + url;
	else
		url = "https://app.terraform.io" + url;
	
	#if DEBUG
	*logs << CYAN << url << RESET << endl;
	#endif

	{
		lock_guard<mutex> lk(curlmutex);
	
		if (!(curl = curl_easy_init()))
			return -1;
		
		if (res = curl_easy_setopt(curl, CURLOPT_URL, url.c_str()))
			return res;

		if (res = curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.c_str()))
			return res;

		if (request == "POST" && post != "")
		{
			if (res = curl_easy_setopt(curl, CURLOPT_POST, 1))
				return res;

			if (res = curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post.c_str()))
				return res;
	 	}
		
		if (res = curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers))
			return res;

		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, callback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &httpData);

		// Optional setting CA bundle... not ideal but libcurl doesn't use env variables.
		if (access("~/tfefs.pem", F_OK) != -1)
			curl_easy_setopt(curl, CURLOPT_CAINFO, "~/tfefs.pem");

		#if DEBUG
		if (post != "")
			*logs << YELLOW << post << RESET << endl;
		#endif 

		curl_easy_perform(curl);
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
		curl_easy_cleanup(curl);
		curl_slist_free_all(headers);
	}

	if (httpCode < 200 || httpCode >= 300)
	{
		*logs << "Couldn't " << request << " " << post << " -> " << url << " HTTP" << httpCode << endl;
		return httpCode;
	}

	return res;
}

int	tfeCURLjson(string url, Json::Value &jsonData, string request = "GET", string post = "")
{
	stringstream stream;
	Json::CharReaderBuilder jsonReader;
	int	res = 0;

	if (res = tfeCURL(url, stream, request, post))
		return res;

	stream >> jsonData;

	return 0;
}

int tfe_getattr(const char *path, struct stat *stat)
{
	const string p(path);
	const size_t slashes = count(p.begin(), p.end(), '/');
	int res = 0;
	Json::Value json, dir;

	stat->st_uid = getuid();
	stat->st_gid = getgid();
	stat->st_blocks = 
	stat->st_blksize = 
	stat->st_size = 0;

	// Be careful with timestamp - file will always appear modified on disk.
	stat->st_atime = stat->st_mtime = stat->st_ctime = time(NULL);
	//stat->st_atime = stat->st_mtime = stat->st_ctime = 0;

	// Some dir levels are actually files.
	if (regex_match(p, (regex)"/organizations/(.*)/workspaces/(.*)/vars")
	||	regex_match(p, (regex)"/organizations/(.*)/policies/(.*)")
	||	regex_match(p, (regex)"/organizations/(.*)/policy-sets/(.*)")
	||  regex_match(p, (regex)"/organizations/(.*)/ssh-keys/(.*)"))
	{
		stat->st_mode = S_IFREG | 0600;
		return 0;
	}

	// Due to crude and ambiguous attrs we can't determine secret or dir
	// In fact, you can have both /path/secret and /path/secret/ which is ugly.
	// FUSE automatically truncates trailing / during traversal.
	// The only way to function is assume one layer deep (1 slash) is a dir, else file.
	if (slashes <= 5)
		stat->st_mode = S_IFDIR | 0700;	// Directory plus execute perm
	else
		stat->st_mode = S_IFREG | 0600;	// File read/write

	return 0;
}

int tfe_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	// To prevent double reads, buffer last read (single thread only!)
	static string buffer;
	string p(path), org, workspace, endpoint;
	const size_t slashes = count(p.begin(), p.end(), '/');
	Json::Value keys;
	int len;

	// Use static buffer to prevent repeat CURL ops
	if (buffer == "")
	{
		stringstream stream;

		// 4: workspaces, policies, policy-sets
		if (slashes > 3)
		{
			// Need to rebase to /api/v2/ and filter on org+ws...
			//?filter%5Bws%5D%5Bname%5D=my-workspace&filter%5Borganization%5D%5Bname%5D=my-organization
			// filter[workspace][name]
			// filter[organization][name]
			// page[number]
			// page[size]
			stringstream sp(p);
			string ignore, type, org, l5, l6, l7;
			getline(sp, ignore, '/');	// /
			getline(sp, ignore, '/');	// orgs
			getline(sp, org, '/');		// JohnBoero
			getline(sp, type, '/');	// workspaces,policies,etc
			getline(sp, l5, '/');		// ws, policy, etc
			getline(sp, l6, '/');		// plans, applies, runs, etc
			getline(sp, l7, '/');		// plan, run, etc

			if (type == "workspaces")
			{
				if (l6 == "vars")
					endpoint = apiVers + "/vars?filter[organization][name]="
						+ org + "&filter[workspace][name]=" + l5;
				else
					endpoint = apiVers + '/' + l6 + "/" + l7;
			}
			else if (regex_match(type, (regex)"policies|policy-sets|ssh-keys"))
				endpoint = apiVers + '/' + type + "/" + basename((char*)path);
			else
				endpoint = apiVers + '/' + (l6.empty()?l5:l6) + "?filter[organization][name]=" + org 
					+ "&filter[workspace][name]=" + basename((char*)path);
		}

		tfeCURL(endpoint, stream);
		buffer = stream.str();
	}

	len = min(size, buffer.length() - offset);

	// We've reached the end of the buffer? (DIRECT_IO)
	if (offset >= len)
	{
		buffer = "";
		return 0;
	}
	
	strncpy(buf + offset, buffer.c_str() + offset, len);
	return len;
}

int tfe_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	string p(path + 1), payload(buf);
	Json::Value mount, data;
	Json::StreamWriterBuilder builder;
	stringstream stream;
	size_t mlen;

	// Need to get mount type to figure out how to read this path.
	//if (res = tfeCURLjson("/v1/sys/mounts", mount))
	//	return -EINVAL;

	if ((mlen = p.find('/')) == string::npos)
		return -ENOTDIR;

	// KV2 Need to re-wrap ourselves in data:{}
	//payload = "{\"data\":" + payload + "}";
	//p.insert(mlen, "/data");

	if (tfeCURL(apiVers + '/' + p, stream, "POST", payload.c_str()))
		return -EINVAL;

	return size;
}

// Return stat of root fs (partition).
int tfe_statfs(const char *path, struct statvfs *statv)
{
	statv->f_bsize	= 
	statv->f_frsize	= 
	statv->f_blocks	= 
	statv->f_bfree	= 
	statv->f_bavail	= 32768;
	statv->f_files	= 15;
	statv->f_bfree	= 15;
	statv->f_favail	= 10000;
	statv->f_fsid	= 100;
	statv->f_flag	= 0;
	statv->f_namemax = 0xFFFF;
	return 0;
}

// Helper function to readdir list json array
void fillArray(Json::Value &array, void *buf, fuse_fill_dir_t filler)
{
	for (Json::Value::ArrayIndex i = 0; i != array.size(); ++i)
	{
		string key = array[i]["id"].asString();
		size_t slash = key.find('/');
		if (slash != string::npos)
			key = key.substr(0, slash);

		filler(buf, key.c_str(), NULL, 0);
	}
}

// Helper function to readdir list json members
void fillMembers(Json::Value &root, void *buf, fuse_fill_dir_t filler)
{
	vector<string> keys = root.getMemberNames();
	for (vector<string>::iterator iter = keys.begin(); iter != keys.end(); ++iter)
	{
		string::size_type pos = iter->find("/");
		if (pos != string::npos)
		    iter->erase(pos);
		filler(buf, iter->c_str(), NULL, 0);
	}
}

// Readdir manually builds out dir structure based on /sys/mounts
// Note that as we're DIRECT_IO and don't necessarily know what's out there,
// we can't use READDIR_PLUS sadly.  I started to implement this in FUSE3 but had issues.
int tfe_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
	string p(path);
	const size_t slashes = count(p.begin(), p.end(), '/');
	Json::Value keys;
	string org, workspace;

	if (p == "/")					// ROOT
		filler(buf, "organizations", NULL, 0);
	else if (slashes == 1)			// /organizations
	{
		// List orgs (GET, not LIST....)
		tfeCURLjson(apiVers + "/organizations", keys);
		fillArray(keys["data"], buf, filler);
	}
	else if (slashes == 2)			// /organizations/JohnBoero
	{
		filler(buf, "workspaces", NULL, 0);
		filler(buf, "policies", NULL, 0);
		filler(buf, "policy-sets", NULL, 0);
		filler(buf, "ssh-keys", NULL, 0);
	}
	else if (slashes == 3)			// /organizations/JohnBoero/workspaces
	{
		// List via GET, not LIST....
		tfeCURLjson(apiVers + p, keys);
		keys = keys["data"];

		for (Json::Value::ArrayIndex i = 0; i != keys.size(); ++i)
		{
			string key = keys[i]["attributes"]["name"].asString();
			size_t slash = key.find('/');
			if (slash != string::npos)
			key = key.substr(0, slash);

			filler(buf, key.c_str(), NULL, 0);
		}

		if (!regex_match(p, (regex)"(.*)/workspaces"))
			fillArray(keys, buf, filler);
	}
	else if (slashes == 4)			// /organizations/JohnBoero/workspaces/test3
	{
		if (regex_match(path, (regex)".*/workspaces/.*"))
		{
			filler(buf, "plans", NULL, 0);
			filler(buf, "applies", NULL, 0);
			filler(buf, "state-versions", NULL, 0);
			filler(buf, "vars", NULL, 0);
			filler(buf, "runs", NULL, 0);
		}
	}
	else if (slashes == 5)			// /organizations/JohnBoero/workspaces/test3/{plans,etc}
	{
		// Need to rebase to /api/v2/ and filter on org+ws...
		//?filter%5Bws%5D%5Bname%5D=my-workspace&filter%5Borganization%5D%5Bname%5D=my-organization
		// filter[workspace][name]
		// filter[organization][name]
		// page[number]
		// page[size]
		stringstream sp(p);
		string ignore, org, ws;
		string endpoint = basename((char*)path);
		getline(sp, ignore, '/');	// /
		getline(sp, ignore, '/');	// orgs
		getline(sp, org, '/');		// JohnBoero
		getline(sp, ignore, '/');	// workspaces
		getline(sp, ws, '/');		// test3

		endpoint = apiVers + '/' + endpoint + "?filter[organization][name]=" + org 
			+ "&filter[workspace][name]=" + ws;

		tfeCURLjson(endpoint, keys);
		fillArray(keys["data"], buf, filler);
	}

	return 0;
}

void* tfe_init(struct fuse_conn_info *conn)
{
	curl_global_init(CURL_GLOBAL_ALL);
	conn->want |= FUSE_CAP_BIG_WRITES;

	return NULL;
}

// Need to implement this for truncate/write.
int tfe_truncate(const char *path, off_t newsize)
{
	return 0;
}

// TODO: Could add tfe metadata and mount types as xattrs.

int main(int argc, char *argv[])
{
	struct fuse_operations fuse = 
	{
		.getattr = tfe_getattr,
		.truncate = tfe_truncate,
		.read = tfe_read,
		.write = tfe_write,
		.statfs = tfe_statfs,
		.readdir = tfe_readdir,
		.init = tfe_init,
	};

	if ((getuid() == 0) || (geteuid() == 0))
		cerr << YELLOW << "WARNING Running a FUSE filesystem as root opens security holes" << endl;
	
	return fuse_main(argc, argv, &fuse, NULL);
}
