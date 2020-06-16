#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "core/ts_helpers_qt.h"
#include "core/ts_serversinfo.h"
#include "core/ts_helpers_qt.h"
#include "core/ts_logging_qt.h"

#include "plugin.h" //pluginID
#include "ts3_functions.h"
#include "core/ts_serversinfo.h"
#include <teamspeak/public_errors.h>

#include "core/plugin_base.h"
#include "mod_radio.h"

#include "tokovoip.h"
#include "client_ws.hpp"

#define CURL_STATICLIB
#include "curl.h"

#include "httplib.h"
using namespace httplib;

Tokovoip *tokovoip;
using WsClient = SimpleWeb::SocketClient<SimpleWeb::WS>;

int isRunning = 0;

HANDLE threadWebSocket = INVALID_HANDLE_VALUE;
shared_ptr<WsClient::Connection> wsConnection;


HANDLE threadSendData = INVALID_HANDLE_VALUE;
HANDLE threadCheckUpdate = INVALID_HANDLE_VALUE;
volatile bool exitSendDataThread = FALSE;

bool isTalking = false;
char* originalName = "";
time_t lastNameSetTick = 0;
string mainChannel = "";
string waitChannel = "";
time_t lastChannelJoin = 0;
string clientIP = "";

time_t noiseWait = 0;

DWORD WINAPI SendDataService(LPVOID lpParam) {
	while (!exitSendDataThread) {
		if (tokovoip->getProcessingState()) {
			Sleep(100);
			continue;
		}
		json data = {
			{ "key", "pluginVersion" },
			{ "value", (string)ts3plugin_version() },
		};
		sendWSMessage("setTS3Data", data);
		Sleep(1000);
	}
	return NULL;
}

int handleMessage(shared_ptr<WsClient::Connection> connection, shared_ptr<WsClient::InMessage> message) {
	int currentPluginStatus = 0;
	tokovoip->setProcessingState(true, currentPluginStatus);
	currentPluginStatus = 1;

	auto message_str = message->string();
	//ts3Functions.logMessage(message_str.c_str(), LogLevel_INFO, "TokoVOIP", 0);


	if (!isConnected(ts3Functions.getCurrentServerConnectionHandlerID()))
	{
		tokovoip->setProcessingState(false, currentPluginStatus);
		return (0);
	}

	DWORD error;
	anyID clientId;
	uint64 serverId = ts3Functions.getCurrentServerConnectionHandlerID();
	std::vector<anyID> clients = getChannelClients(serverId, getCurrentChannel(serverId));
	string thisChannelName = getChannelName(serverId, getMyId(serverId));

	//--------------------------------------------------------

	// Check if connected to any channel
	if (thisChannelName == "")
	{
		tokovoip->setProcessingState(false, currentPluginStatus);
		return (0);
	}

	//--------------------------------------------------------

	// Load the json //
	json json_data = json::parse(message_str.c_str(), nullptr, false);
	if (json_data.is_discarded()) {
		ts3Functions.logMessage("Invalid JSON data", LogLevel_INFO, "TokoVOIP", 0);
		tokovoip->setProcessingState(false, currentPluginStatus);
		return (0);
	}

	json data = json_data["Users"];
	string channelName = json_data["TSChannel"];
	string channelPass = json_data["TSPassword"];
	string waitingChannelName = json_data["TSChannelWait"];
	mainChannel = channelName;
	waitChannel = waitingChannelName;

	bool radioTalking = json_data["radioTalking"];
	bool radioClicks = json_data["localRadioClicks"];
	bool local_click_on = json_data["local_click_on"];
	bool local_click_off = json_data["local_click_off"];
	bool remote_click_on = json_data["remote_click_on"];
	bool remote_click_off = json_data["remote_click_off"];

	//--------------------------------------------------------

	if (isChannelWhitelisted(json_data, thisChannelName)) {
		resetClientsAll();
		currentPluginStatus = 3;
		tokovoip->setProcessingState(false, currentPluginStatus);
		return (0);
	}

	// Check if right channel
	if (channelName != thisChannelName) {
		if (originalName != "")
			setClientName(originalName);

		// Handle auto channel switch
		if (thisChannelName == waitingChannelName)
		{
			if (noiseWait == 0 || (time(nullptr) - noiseWait) > 1)
				noiseWait = time(nullptr);
			uint64* result;
			if ((error = ts3Functions.getChannelList(serverId, &result)) != ERROR_ok)
			{
				outputLog("Can't get channel list", error);
			}
			else
			{
				bool joined = false;
				uint64* iter = result;
				while (*iter && !joined && (time(nullptr) - lastChannelJoin) > 1)
				{
					uint64 channelId = *iter;
					iter++;
					char* cName;
					if ((error = ts3Functions.getChannelVariableAsString(serverId, channelId, CHANNEL_NAME, &cName)) != ERROR_ok) {
						outputLog("Can't get channel name", error);
					}
					else
					{
						if (!strcmp(channelName.c_str(), cName))
						{
							if (time(nullptr) - noiseWait < 1)
							{
								std::vector<anyID> channelClients = getChannelClients(serverId, channelId);
								for (auto clientIdIterator = channelClients.begin(); clientIdIterator != channelClients.end(); clientIdIterator++)
								{
									setClientMuteStatus(serverId, *clientIdIterator, true);
								}
								tokovoip->setProcessingState(false, currentPluginStatus);
								return (0);
							}
							lastChannelJoin = time(nullptr);
							if ((error = ts3Functions.requestClientMove(serverId, getMyId(serverId), channelId, channelPass.c_str(), NULL)) != ERROR_ok) {
								outputLog("Can't join channel", error);
								currentPluginStatus = 2;
								tokovoip->setProcessingState(false, currentPluginStatus);
								return (0);
							}
							else
							{
								joined = true;
							}
						}
						ts3Functions.freeMemory(cName);
					}
				}
				ts3Functions.freeMemory(result);
			}
		}
		else
		{
			resetClientsAll();
			currentPluginStatus = 2;
			tokovoip->setProcessingState(false, currentPluginStatus);
			return (0);
		}
	}

	serverId = ts3Functions.getCurrentServerConnectionHandlerID();

	// Save client's original name
	if (originalName == "")
		if ((error = ts3Functions.getClientVariableAsString(serverId, getMyId(serverId), CLIENT_NICKNAME, &originalName)) != ERROR_ok) {
			outputLog("Error getting client nickname", error);
			tokovoip->setProcessingState(false, currentPluginStatus);
			return (0);
		}

	// Set client's name to ingame name
	string newName = originalName;

	if (json_data.find("localName") != json_data.end()) {
		string localName = json_data["localName"];
		if (localName != "") newName = localName;
	}

	if (json_data.find("localNamePrefix") != json_data.end()) {
		string localNamePrefix = json_data["localNamePrefix"];
		if (localNamePrefix != "") newName = localNamePrefix + newName;
	}

	if (newName != "") {
		setClientName(newName);
	}

	// Activate input if talking on radio
	if (radioTalking)
	{
		setClientTalking(true);
		if (!isTalking && radioClicks == true && local_click_on == true)
			playWavFile("mic_click_on");
		isTalking = true;
	}
	else
	{
		if (isTalking)
		{
			setClientTalking(false);
			if (radioClicks == true && local_click_off == true)
				playWavFile("mic_click_off");
			isTalking = false;
		}
	}

	// Handle positional audio
	if (json_data.find("posX") != json_data.end() && json_data.find("posY") != json_data.end() && json_data.find("posZ") != json_data.end()) {
		TS3_VECTOR myPosition;
		myPosition.x = (float)json_data["posX"];
		myPosition.y = (float)json_data["posY"];
		myPosition.z = (float)json_data["posZ"];
		ts3Functions.systemset3DListenerAttributes(serverId, &myPosition, NULL, NULL);
	}

	// Process other clients
	for (auto clientIdIterator = clients.begin(); clientIdIterator != clients.end(); clientIdIterator++) {
		clientId = *clientIdIterator;
		char *UUID;
		if ((error = ts3Functions.getClientVariableAsString(serverId, clientId, CLIENT_UNIQUE_IDENTIFIER, &UUID)) != ERROR_ok) {
			outputLog("Error getting client UUID", error);
		} else {
			bool foundPlayer = false;
			if (clientId == getMyId(serverId)) {
				foundPlayer = true;
				continue;
			}
			for (auto user : data) {
				if (!user.is_object()) continue;
				if (!user["uuid"].is_string()) continue;

				string gameUUID = user["uuid"];
				int muted = user["muted"];
				float volume = user["volume"];
				bool isRadioEffect = user["radioEffect"];

				if (channelName == thisChannelName && UUID == gameUUID) {
					foundPlayer = true;
					if (isRadioEffect == true && tokovoip->getRadioData(UUID) == false && remote_click_on == true)
						playWavFile("mic_click_on");
					if (remote_click_off == true && isRadioEffect == false && tokovoip->getRadioData(UUID) == true && clientId != getMyId(serverId))
						playWavFile("mic_click_off");
					tokovoip->setRadioData(UUID, isRadioEffect);
					if (muted)
						setClientMuteStatus(serverId, clientId, true);
					else {
						setClientMuteStatus(serverId, clientId, false);
						ts3Functions.setClientVolumeModifier(serverId, clientId, volume);
						if (json_data.find("posX") != json_data.end() && json_data.find("posY") != json_data.end() && json_data.find("posZ") != json_data.end()) {
							TS3_VECTOR Vector;
							Vector.x = (float)user["posX"];
							Vector.y = (float)user["posY"];
							Vector.z = (float)user["posZ"];
							ts3Functions.channelset3DAttributes(serverId, clientId, &Vector);
						}
					}
				}
			};
			// Checks if ts3 user is on fivem server. Fixes onesync infinity issues as big mode is using
			// player culling (Removes a player from your game if he is far away), this fix should mute him when he is removed from your game (too far away)

			// Also keep in mind teamspeak3 defautly mutes everyone in channel if theres more than 100 people,
			// this can be changed in the server settings -> Misc -> Min clients in channel before silence

			if (!foundPlayer) setClientMuteStatus(serverId, clientId, true);
			ts3Functions.freeMemory(UUID);
		}
	}
	currentPluginStatus = 3;
	tokovoip->setProcessingState(false, currentPluginStatus);
}

int tries = 0;
DWORD WINAPI WebSocketService(LPVOID lpParam) {
	while (true) {
		uint64 serverId = ts3Functions.getCurrentServerConnectionHandlerID();
		if (isConnected(serverId)) break;
		Sleep(1000);
	}

	/*string endpoint = getWebSocketEndpoint();
	if (endpoint == "") {
		outputLog("Failed to retrieve the websocket endpoint, too many tries. Restart TS3 to try again.");
		return NULL;
	}*/
	
	//WsClient client(endpoint);
	WsClient client("localhost:3000/socket.io/?EIO=3&transport=websocket");

	client.on_message = [](shared_ptr<WsClient::Connection> connection, shared_ptr<WsClient::InMessage> in_message) {
		outputLog("Websocket message received:" + in_message->string());
	};

	client.on_open = [](shared_ptr<WsClient::Connection> connection) {
		outputLog("Websocket connection opened");
		wsConnection = connection;

		json data = {
			{ "key", "pluginVersion" },
			{ "value", (string)ts3plugin_version() },
		};
		sendWSMessage("setTS3Data", data);

		uint64 serverId = ts3Functions.getCurrentServerConnectionHandlerID();
		char *UUID;
		if (ts3Functions.getClientSelfVariableAsString(serverId, CLIENT_UNIQUE_IDENTIFIER, &UUID) == ERROR_ok) {
			json data = {
				{ "key", "uuid" },
				{ "value", (string)UUID },
			};
			sendWSMessage("setTS3Data", data);
		}
		free(UUID);
	};

	client.on_close = [&](shared_ptr<WsClient::Connection>, int status, const string &) {
		outputLog("Websocket connection closed: status " + status);
		client.stop();
		resetState();
		initWebSocket();
	};

	client.on_error = [&](shared_ptr<WsClient::Connection>, const SimpleWeb::error_code &ec) {
		outputLog("Websocket error: " + ec.message());
		client.stop();
		resetState();
		initWebSocket();
	};

	client.start();
	return NULL;
}

void initWebSocket() {
	outputLog("Initializing WebSocket Thread", 0);
	threadWebSocket = CreateThread(NULL, 0, WebSocketService, NULL, 0, NULL);
}

void initDataThread() {
	outputLog("Initializing Data Thread", 0);
	exitSendDataThread = false;
	threadSendData = CreateThread(NULL, 0, SendDataService, NULL, 0, NULL);
}

string getWebSocketEndpoint() {
	int sleepLength = 5000;

	json serverInfo = NULL;
	json fivemServer = NULL;

	sleepLength = 5000;
	tries = 0;
	while (clientIP == "") {
		tries += 1;
		outputLog("Requesting client IP (attempt " + to_string(tries) + ")");
		httplib::Client cli("api.ipify.org");
		cli.set_follow_location(true);
		auto res = cli.Get("/");
		if (res && res->status == 200) clientIP = res->body;
		if (clientIP == "") {
			Sleep(sleepLength);
			if (tries >= 2) sleepLength += 5000;
			if (tries >= 5) {
				outputLog("Could not retrieve the client IP");
				return "";
			}
		}
	}

	sleepLength = 5000;
	tries = 0;
	while (serverInfo == NULL) {
		tries += 1;
		outputLog("Requesting server info (attempt " + to_string(tries) + ")");
		serverInfo = getServerInfoFromMaster();
		if (serverInfo == NULL) {
			outputLog("No server info found");
			Sleep(sleepLength);
			if (tries >= 2) sleepLength += 5000;
			if(tries >= 5) {
				outputLog("Could not retrieve the server info");
				return "";
			}
		}
	}

	outputLog("Retrieved server info");

	sleepLength = 5000;
	tries = 0;
	while (fivemServer == NULL) {
		tries += 1;
		outputLog("Requesting fivem server info (attempt " + to_string(tries) + ")");
		fivemServer = getServerFromClientIP(serverInfo["servers"]);
		if (fivemServer == NULL) {
			outputLog("No server found");
			Sleep(sleepLength);
			if (tries >= 20) sleepLength = 10000;
			if (tries >= 40) {
				outputLog("Could not retrieve the fivem server info");
				return "";
			}
		}
	}

	outputLog("Retrieved fivem server info");

	string fivemServerIP = fivemServer["ip"];
	string fivemServerPORT = fivemServer["port"];

	outputLog(fivemServerIP + ":" + fivemServerPORT);

	return fivemServerIP + ":" + fivemServerPORT + "/socket.io/?EIO=3&transport=websocket";
}

void resetChannel() {
	uint64 serverId = ts3Functions.getCurrentServerConnectionHandlerID();
	uint64* result;
	DWORD error;
	if ((error = ts3Functions.getChannelList(serverId, &result)) != ERROR_ok) {
		outputLog("resetChannel: Can't get channel list", error);
		return;
	}

	uint64* iter = result;
	while (*iter) {
		uint64 channelId = *iter;
		iter++;
		char* cName;
		if ((error = ts3Functions.getChannelVariableAsString(serverId, channelId, CHANNEL_NAME, &cName)) != ERROR_ok) {
			outputLog("resetChannel: Can't get channel name", error);
			continue;
		}
		if (!strcmp(waitChannel.c_str(), cName)) {
			if ((error = ts3Functions.requestClientMove(serverId, getMyId(serverId), channelId, "", NULL)) != ERROR_ok) {
				outputLog("resetChannel: Can't join channel", error);
				tokovoip->setProcessingState(false, 0);
				return;
			}
			break;
		}
		ts3Functions.freeMemory(cName);
	}
	ts3Functions.freeMemory(result);
}

void resetState() {
	wsConnection = NULL;
	exitSendDataThread = false;
	uint64 serverId = ts3Functions.getCurrentServerConnectionHandlerID();
	string currentChannelName = getChannelName(serverId, getMyId(serverId));
	if (mainChannel == currentChannelName) resetChannel();
	resetClientsAll();
	if (originalName != "") setClientName(originalName);
}


void sendWSMessage(string endpoint, json value) {
	if (!wsConnection) return;

	json send = json::array({ endpoint, value });
	wsConnection->send("42" + send.dump());
}

// callback function writes data to a std::ostream
static size_t data_write(void* buf, size_t size, size_t nmemb, void* userp)
{
	if (userp)
	{
		std::ostream& os = *static_cast<std::ostream*>(userp);
		std::streamsize len = size * nmemb;
		if (os.write(static_cast<char*>(buf), len))
			return len;
	}

	return 0;
}

size_t curl_callback(const char* in, size_t size, size_t num, string* out) {
	const size_t totalBytes(size * num);
	out->append(in, totalBytes);
	return totalBytes;
}

json downloadJSON(string url) {
	CURL* curl = curl_easy_init();

	// Set remote URL.
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

	// Don't bother trying IPv6, which would increase DNS resolution time.
	curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);

	// Don't wait forever, time out after 10 seconds.
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10);

	// Follow HTTP redirects if necessary.
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

	// Response information.
	int httpCode(0);
	unique_ptr<string> httpData(new string());

	// Hook up data handling function.
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_callback);

	// Hook up data container (will be passed as the last parameter to the
	// callback handling function).  Can be any pointer type, since it will
	// internally be passed as a void pointer.
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, httpData.get());

	// Run our HTTP GET command, capture the HTTP response code, and clean up.
	curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
	curl_easy_cleanup(curl);

	if (httpCode == 200) {
		// Response looks good - done using Curl now.  Try to parse the results
		// and print them out.
		json parsedJSON = json::parse(*httpData.get(), nullptr, false);
		if (parsedJSON.is_discarded()) {
			outputLog("Downloaded JSON is invalid");
			return NULL;
		}
		return parsedJSON;
	} else {
		outputLog("Couldn't retrieve JSON (Code: " + to_string(httpCode) + ")");
		return NULL;
	}

	return NULL;
}

void checkUpdate() {
	json updateJSON = downloadJSON("http://itokoyamato.net/files/tokovoip/tokovoip_info.json");
	if (updateJSON != NULL) {
		outputLog("Got update json");
	}

	if (updateJSON == NULL || updateJSON.find("minVersion") == updateJSON.end() || updateJSON.find("currentVersion") == updateJSON.end()) {
		outputLog("Invalid update JSON");
		Sleep(600000); // Don't check for another 10mins
		return;
	}

	string minVersion = updateJSON["minVersion"];
	string minVersionNum = updateJSON["minVersion"];
	minVersionNum.erase(remove(minVersionNum.begin(), minVersionNum.end(), '.'), minVersionNum.end());

	string currentVersion = updateJSON["currentVersion"];
	string currentVersionNum = updateJSON["currentVersion"];
	currentVersionNum.erase(remove(currentVersionNum.begin(), currentVersionNum.end(), '.'), currentVersionNum.end());

	string myVersion = ts3plugin_version();
	myVersion.erase(remove(myVersion.begin(), myVersion.end(), '.'), myVersion.end());

	string updateMessage;
	if (updateJSON.find("versions") != updateJSON.end() &&
		updateJSON["versions"].find(currentVersion) != updateJSON["versions"].end() &&
		updateJSON["versions"][currentVersion].find("updateMessage") != updateJSON["versions"][currentVersion].end()) {

		string str = updateJSON["versions"][currentVersion]["updateMessage"];
		updateMessage = str;
	} else {
		string str = updateJSON["defaultUpdateMessage"];
		updateMessage = str;
	}

	if (myVersion < currentVersionNum) {
		string url = "";
		if (updateJSON.find("versions") != updateJSON.end() &&
			updateJSON["versions"].find(currentVersion) != updateJSON["versions"].end() &&
			updateJSON["versions"][currentVersion].find("updateUrl") != updateJSON["versions"][currentVersion].end()) {

			string tmp = updateJSON["versions"][currentVersion]["updateUrl"];
			url = tmp;
		}

		MessageBox(NULL, updateMessage.c_str(), "TokoVOIP: update", MB_OK);

		if (myVersion < minVersionNum && updateJSON.find("minVersionWarningMessage") != updateJSON.end() && !updateJSON["minVersionWarningMessage"].is_null()) {
			string minVersionWarningMessage = updateJSON["minVersionWarningMessage"];
			MessageBox(NULL, minVersionWarningMessage.c_str(), "TokoVOIP: update", MB_OK);
		}
	}
}

json getServerInfoFromMaster() {
	uint64 serverId = ts3Functions.getCurrentServerConnectionHandlerID();
	unsigned int error;
	char* serverIP;

	if ((error = ts3Functions.getConnectionVariableAsString(serverId, getMyId(ts3Functions.getCurrentServerConnectionHandlerID()), CONNECTION_SERVER_IP, &serverIP)) != ERROR_ok) {
		if (error != ERROR_not_connected) ts3Functions.logMessage("Error querying server name", LogLevel_ERROR, "Plugin", serverId);
		return NULL;
	}

	httplib::Client cli("localhost", 3005);
	string path = "/server?address=" + string(serverIP);
	auto res = cli.Get(path.c_str());
	if (res && res->status == 200) {
		json parsedJSON = json::parse(res->body, nullptr, false);
		if (parsedJSON.is_discarded()) {
			outputLog("ServerInfo: invalid JSON");
			return NULL;
		}
		return parsedJSON;
	 }
	return NULL;
}

json getServerFromClientIP(json servers) {
	uint64 serverId = ts3Functions.getCurrentServerConnectionHandlerID();
	unsigned int error;

	if (clientIP == "") return NULL;

	int serverCount = 0;
	for (json::iterator it = servers.begin(); it != servers.end(); ++it) {
		json JSON = *it;
		if (JSON.find("ip") == JSON.end() || JSON.find("port") == JSON.end()) continue;
		string ip = JSON["ip"];
		string port = JSON["port"];

		serverCount += 1;
		outputLog("Requesting fivem server " + to_string(serverCount));
		httplib::Client cli(ip, atoi(port.c_str()));
		string path = "/playerbyip?ip=" + string(clientIP);
		auto res = cli.Get(path.c_str());
		if (res && res->status == 204) return JSON;
		outputLog("Client not connected to fivem server " + to_string(serverCount));
	}
	return NULL;
}

int Tokovoip::initialize(char *id) {
	plugin_id = id;
	const int sz = strlen(id) + 1;
	plugin_id = (char*)malloc(sz * sizeof(char));
	strcpy(plugin_id, id);
	if (isRunning != 0)
		return (0);
	outputLog("TokoVOIP initialized", 0);

	resetClientsAll();
	checkUpdate();
	isRunning = false;
	tokovoip = this;
	//exitTimeoutThread = false;
	//exitSendDataThread = false;
	//threadService = CreateThread(NULL, 0, ServiceThread, NULL, 0, NULL);
	//threadTimeout = CreateThread(NULL, 0, TimeoutThread, NULL, 0, NULL);
	//threadSendData = CreateThread(NULL, 0, SendDataThread, NULL, 0, NULL);
	initWebSocket();
	return (1);
}

void Tokovoip::shutdown()
{
	//exitTimeoutThread = true;
	//exitSendDataThread = true;
	//resetClientsAll();

	//DWORD exitCode;
	//BOOL result = GetExitCodeThread(threadService, &exitCode);
	//if (!result || exitCode == STILL_ACTIVE)
	//	outputLog("service thread not terminated", LogLevel_CRITICAL);
}

vector<string> explode(const string& str, const char& ch) {
	string next;
	vector<string> result;

	// For each character in the string
	for (string::const_iterator it = str.begin(); it != str.end(); it++) {
		// If we've hit the terminal character
		if (*it == ch) {
			// If we have some characters accumulated
			if (!next.empty()) {
				// Add them to the result vector
				result.push_back(next);
				next.clear();
			}
		}
		else {
			// Accumulate the next character into the sequence
			next += *it;
		}
	}
	if (!next.empty())
		result.push_back(next);
	return result;
}

// Utils

bool isChannelWhitelisted(json data, string channel) {
	if (data == NULL) return false;
	if (data.find("TSChannelWhitelist") == data.end()) return false;
	for (json::iterator it = data["TSChannelWhitelist"].begin(); it != data["TSChannelWhitelist"].end(); ++it) {
		string whitelistedChannel = it.value();
		if (whitelistedChannel == channel) return true;
	}
	return false;
}

void playWavFile(const char* fileNameWithoutExtension)
{
	char pluginPath[PATH_BUFSIZE];
	ts3Functions.getPluginPath(pluginPath, PATH_BUFSIZE, tokovoip->getPluginID());
	std::string path = std::string((string)pluginPath);
	DWORD error;
	std::string to_play = path + "tokovoip/" + std::string(fileNameWithoutExtension) + ".wav";
	if ((error = ts3Functions.playWaveFile(ts3Functions.getCurrentServerConnectionHandlerID(), to_play.c_str())) != ERROR_ok)
	{
		outputLog("can't play sound", error);
	}
}

void	setClientName(string name)
{
	DWORD error;
	char* currentName;
	uint64 serverId = ts3Functions.getCurrentServerConnectionHandlerID();

	// Cancel name change is anti-spam timer still active
	if (time(nullptr) - lastNameSetTick < 2) return;

	lastNameSetTick = time(nullptr);

	if ((error = ts3Functions.flushClientSelfUpdates(serverId, NULL)) != ERROR_ok && error != ERROR_ok_no_update)
		return outputLog("Can't flush self updates.", error);

	if ((error = ts3Functions.getClientVariableAsString(serverId, getMyId(serverId), CLIENT_NICKNAME, &currentName)) != ERROR_ok)
		return outputLog("Error getting client nickname", error);

	// Cancel name changing if name is already the same
	if (name == (string)currentName) return;

	// Set name
	if ((error = ts3Functions.setClientSelfVariableAsString(serverId, CLIENT_NICKNAME, name.c_str())) != ERROR_ok)
		return outputLog("Error setting client nickname", error);

	if ((error = ts3Functions.flushClientSelfUpdates(serverId, NULL)) != ERROR_ok && error != ERROR_ok_no_update)
		outputLog("Can't flush self updates.", error);
}

void setClientTalking(bool status)
{
	DWORD error;
	uint64 serverId = ts3Functions.getCurrentServerConnectionHandlerID();
	if (status)
	{
		if ((error = ts3Functions.setClientSelfVariableAsInt(serverId, CLIENT_INPUT_DEACTIVATED, 0)) != ERROR_ok)
			outputLog("Can't active input.", error);
		error = ts3Functions.flushClientSelfUpdates(serverId, NULL);
		if (error != ERROR_ok && error != ERROR_ok_no_update)
			outputLog("Can't flush self updates.", error);
	}
	else
	{
		if ((error = ts3Functions.setClientSelfVariableAsInt(serverId, CLIENT_INPUT_DEACTIVATED, 1)) != ERROR_ok)
			outputLog("Can't deactive input.", error);
		error = ts3Functions.flushClientSelfUpdates(serverId, NULL);
		if (error != ERROR_ok && error != ERROR_ok_no_update)
			outputLog("Can't flush self updates.", error);
	}
}

void setClientMuteStatus(uint64 serverConnectionHandlerID, anyID clientId, bool status)
{
	anyID clientIds[2];
	clientIds[0] = clientId;
	clientIds[1] = 0;
	if (clientIds[0] <= 0)
		return;
	DWORD error;
	int isLocalMuted;
	if ((error = ts3Functions.getClientVariableAsInt(ts3Functions.getCurrentServerConnectionHandlerID(), clientId, CLIENT_IS_MUTED, &isLocalMuted)) != ERROR_ok) {
		outputLog("Error getting client nickname", error);
	}
	else
	{
		if (status && isLocalMuted == 0)
		{
			if ((error = ts3Functions.requestMuteClients(serverConnectionHandlerID, clientIds, NULL)) != ERROR_ok)
				outputLog("Can't mute client", error);
		}
		if (!status && isLocalMuted == 1)
		{
			if ((error = ts3Functions.requestUnmuteClients(serverConnectionHandlerID, clientIds, NULL)) != ERROR_ok)
				outputLog("Can't unmute client", error);
		}
	}
}

void outputLog(string message, DWORD errorCode)
{
	string output = message;
	if (errorCode != NULL) {
		char* errorBuffer;
		ts3Functions.getErrorMessage(errorCode, &errorBuffer);
		output = output + " : " + string(errorBuffer);
		ts3Functions.freeMemory(errorBuffer);
	}

	ts3Functions.logMessage(output.c_str(), LogLevel_INFO, "TokoVOIP", 141);
}

void resetVolumeAll(uint64 serverConnectionHandlerID)
{
	vector<anyID> clientsIds = getChannelClients(serverConnectionHandlerID, getCurrentChannel(serverConnectionHandlerID));
	anyID myId = getMyId(serverConnectionHandlerID);
	DWORD error;
	char *UUID;

	for (auto clientId = clientsIds.begin(); clientId != clientsIds.end(); clientId++) {
		if (*clientId != myId) {
			ts3Functions.setClientVolumeModifier(serverConnectionHandlerID, (*clientId), 0.0f);
			if ((error = ts3Functions.getClientVariableAsString(serverConnectionHandlerID, *clientId, CLIENT_UNIQUE_IDENTIFIER, &UUID)) != ERROR_ok) {
				outputLog("Error getting client UUID", error);
			} else {
				tokovoip->setRadioData(UUID, false);
			}
		}
	}
}

void unmuteAll(uint64 serverConnectionHandlerID)
{
	anyID* ids;
	DWORD error;

	if ((error = ts3Functions.getClientList(serverConnectionHandlerID, &ids)) != ERROR_ok)
	{
		outputLog("Error getting all clients from server", error);
		return;
	}

	if ((error = ts3Functions.requestUnmuteClients(serverConnectionHandlerID, ids, NULL)) != ERROR_ok)
	{
		outputLog("Can't unmute all clients", error);
	}
	ts3Functions.freeMemory(ids);
}

void resetPositionAll(uint64 serverConnectionHandlerID)
{
	vector<anyID> clientsIds = getChannelClients(serverConnectionHandlerID, getCurrentChannel(serverConnectionHandlerID));
	anyID myId = getMyId(serverConnectionHandlerID);

	ts3Functions.systemset3DListenerAttributes(serverConnectionHandlerID, NULL, NULL, NULL);

	for (auto clientId = clientsIds.begin(); clientId != clientsIds.end(); clientId++)
	{
		if (*clientId != myId) ts3Functions.channelset3DAttributes(serverConnectionHandlerID, *clientId, NULL);
	}
}

void resetClientsAll() {
	uint64 serverConnectionHandlerID = ts3Functions.getCurrentServerConnectionHandlerID();
	resetPositionAll(serverConnectionHandlerID);
	resetVolumeAll(serverConnectionHandlerID);
	unmuteAll(serverConnectionHandlerID);
}

vector<anyID> getChannelClients(uint64 serverConnectionHandlerID, uint64 channelId)
{
	DWORD error;
	vector<anyID> result;
	anyID* clients = NULL;
	if ((error = ts3Functions.getChannelClientList(serverConnectionHandlerID, channelId, &clients)) == ERROR_ok) {
		int i = 0;
		while (clients[i]) {
			result.push_back(clients[i]);
			i++;
		}
		ts3Functions.freeMemory(clients);
	} else {
		outputLog("Error getting all clients from the server", error);
	}
	return result;
}

uint64 getCurrentChannel(uint64 serverConnectionHandlerID)
{
	uint64 channelId;
	DWORD error;
	if ((error = ts3Functions.getChannelOfClient(serverConnectionHandlerID, getMyId(serverConnectionHandlerID), &channelId)) != ERROR_ok)
		outputLog("Can't get current channel", error);
	return channelId;
}

anyID getMyId(uint64 serverConnectionHandlerID)
{
	anyID myID = (anyID)-1;
	if (!isConnected(serverConnectionHandlerID)) return myID;
	DWORD error;
	if ((error = ts3Functions.getClientID(serverConnectionHandlerID, &myID)) != ERROR_ok)
		outputLog("Failure getting client ID", error);
	return myID;
}

std::string getChannelName(uint64 serverConnectionHandlerID, anyID clientId)
{
	if (clientId == (anyID)-1) return "";
	uint64 channelId;
	DWORD error;
	if ((error = ts3Functions.getChannelOfClient(serverConnectionHandlerID, clientId, &channelId)) != ERROR_ok)
	{
		outputLog("Can't get channel of client", error);
		return "";
	}
	char* channelName;
	if ((error = ts3Functions.getChannelVariableAsString(serverConnectionHandlerID, channelId, CHANNEL_NAME, &channelName)) != ERROR_ok) {
		outputLog("Can't get channel name", error);
		return "";
	}
	std::string result = std::string(channelName);
	ts3Functions.freeMemory(channelName);
	return result;
}

bool isConnected(uint64 serverConnectionHandlerID)
{
	DWORD error;
	int result;
	if ((error = ts3Functions.getConnectionStatus(serverConnectionHandlerID, &result)) != ERROR_ok)
		return false;
	return result != 0;
}
