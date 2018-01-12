#include <unistd.h>

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <condition_variable>
#include <sstream>
#include <iostream>
#include <curlpp/cURLpp.hpp>
#include <curlpp/Easy.hpp>
#include <curlpp/Options.hpp>
#include <sio_client.h>
#include "rapidjson/document.h"
#include "rxcpp/rx.hpp"
#include "base64.h"
#include "proto3/albia.pb.h"
#include "proto3/timestamp.pb.h"

using namespace sio;
using namespace rapidjson;
using namespace std;

std::mutex _lock;
std::condition_variable_any _cond;

class SIOConnectionListener
{
    sio::client *handler;

public:

    bool connectFinished;

    SIOConnectionListener(sio::client* h):
    handler(h)
    {
      this->connectFinished = false;
    }

    void on_connected()
    {
        _lock.lock();
        _cond.notify_all();
        this->connectFinished = true;
        _lock.unlock();
    }
    void on_close(client::close_reason const& reason)
    {
        exit(0);
    }

    void on_fail()
    {
        exit(0);
    }
};

class DeviceClient {

public:

struct device_token_t {
        string token;
        unsigned int device_id;
};

bool isConnected;
DeviceClient(const string& apiKey, const string& deviceKey, const string& hostname);
void connect(const string& hostname);
void reconnect();
void disconnect();
void onConnect(const std::function<void()>& callback);
void onConnectError(const std::function<void(const std::exception& ex)>& callback);
void onDisconnect(const std::function<void()>& callback);
rxcpp::observable<int> connectToServer(const string& hostname, unsigned int apiPort, unsigned int webSocketPort, const string& deviceToken, const string& apiKey, const string& deviceKey);

private:

string apiKey;
string deviceKey;
string host;
unsigned int apiPort;
unsigned int webSocketPort;
string dbFilename;
string deviceToken;
unsigned int deviceId;
string socketIOnamespace;
sio::client *socketIO;
sio::socket::ptr currentSocket;
SIOConnectionListener *connectionListener;
static const vector<string> explode(const string& s, const char& c);
std::function<void()> onConnectCallback;
std::function<void(const std::exception& ex)> onConnectErrorCallback;
std::function<void()> onDisconnectCallback;
device_token_t* getDeviceTokenWithAPIKeyAndDeviceKey(const string& hostname, unsigned int apiPort, const string& apiKey, const string& deviceKey);

};

DeviceClient::DeviceClient(const string& apiKey, const string& deviceKey, const string& hostname) {
        this->isConnected = false;
        this->apiKey = apiKey;
        this->deviceKey = deviceKey;
        this->host = hostname;
        this->apiPort = 3001;
        this->webSocketPort = 3000;
        this->deviceId = 0;
        this->dbFilename = "albia.sqlite";
        this->deviceToken = "";
        this->socketIOnamespace = "";
        this->onConnectCallback = NULL;
        this->onConnectErrorCallback = NULL;
        this->socketIO = NULL;
        this->connectionListener = NULL;
        this->currentSocket = NULL;
}

const vector<string> DeviceClient::explode(const string& s, const char& c) {
        string buff{""};
        vector<string> v;

        for(auto n:s) {
                if(n != c) buff+=n; else
                if(n == c && buff != "") { v.push_back(buff); buff = ""; }
        }
        if(buff != "") v.push_back(buff);

        return v;
}

void DeviceClient::onConnect(const std::function<void()>& callback) {
   this->onConnectCallback = callback;
}

void DeviceClient::onConnectError(const std::function<void(const std::exception&)>& callback) {
   this->onConnectErrorCallback = callback;
}

void DeviceClient::onDisconnect(const std::function<void()>& callback) {
   this->onDisconnectCallback = callback;
}

DeviceClient::device_token_t* DeviceClient::getDeviceTokenWithAPIKeyAndDeviceKey(const string& hostname, unsigned int apiPort, const string& apiKey, const string& deviceKey) {

        try {
                curlpp::Cleanup cleaner;
                curlpp::Easy request;
                string scheme("http://");
                string endpoint("/v1/request-device-token");

                curlpp::options::Url serverUrl(scheme+hostname+endpoint);
                curlpp::options::Port serverPort(apiPort);

                list<string> headersList;
                headersList.push_back("Accept: application/json");
                headersList.push_back("X-albia-api-key: "+apiKey);
                headersList.push_back("X-albia-device-key: "+deviceKey);
                curlpp::options::HttpHeader headers(headersList);

                ostringstream os;
                curlpp::options::WriteStream ws(&os);
                request.setOpt(ws);
                request.setOpt(headers);
                request.setOpt(serverUrl);
                request.setOpt(serverPort);
                request.perform();

                string jsonResponse = os.str();
                cout << "RESPONSE: " << jsonResponse << endl;
                Document d;
                d.Parse(jsonResponse.c_str());
                Value& s = d["token"];

                device_token_t* deviceToken = new device_token_t;
                deviceToken->token = s.GetString();

                string decodedToken = base64_decode(deviceToken->token);
                vector<string> tokenVector = DeviceClient::explode(decodedToken, ';');
                deviceToken->device_id = stoi(tokenVector[0]);

                return deviceToken;

        } catch( curlpp::RuntimeError &e ) {
                cout << e.what() << endl;
        } catch( curlpp::LogicError &e ) {
                cout << e.what() << endl;
        }

        return NULL;
}

void DeviceClient::connect(const string& hostname = "") {

  if(this->isConnected) {
    return;
  }

  if(hostname != "") {
    this->host = hostname;
  }

  rxcpp::observable<int> observable = this->connectToServer(this->host, this->apiPort, this->webSocketPort, this->deviceToken, this->apiKey, this->deviceKey);

  observable.subscribe(
          [](int v) {
  },
          [this](std::exception_ptr ep){
          this->isConnected = false;
          try {std::rethrow_exception(ep);}
          catch (const std::exception& ex) {
                  if(this->onConnectErrorCallback != NULL) {
                    (this->onConnectErrorCallback)(ex);
                  }
          }
  },
          [this](){

            if(this->socketIO != NULL) {
              delete this->socketIO;
            }

            if(this->connectionListener != NULL) {
              delete this->connectionListener;
            }

            if(this->currentSocket != NULL) {
              delete &this->currentSocket;
            }

            cout << endl << "Creant instancia de sio::client" << endl;
            this->socketIO = new sio::client();
            cout << endl << "Creant instancia de SIOConnectionListener" << endl;
            this->connectionListener = new SIOConnectionListener(this->socketIO);

            this->socketIO->set_open_listener(std::bind(&SIOConnectionListener::on_connected, this->connectionListener));
            this->socketIO->set_close_listener(std::bind(&SIOConnectionListener::on_close, this->connectionListener,std::placeholders::_1));
            this->socketIO->set_fail_listener(std::bind(&SIOConnectionListener::on_fail, this->connectionListener));

            cout << endl << "Connectant websocket... " << "http://"+this->host+":"+std::to_string(this->webSocketPort) << endl;
            std::map<std::string, std::string> httpHeaders;
            httpHeaders["authorization"] = this->deviceToken;
            this->socketIO->connect("http://"+this->host+":"+std::to_string(this->webSocketPort), {}, httpHeaders);

            _lock.lock();
            if(!this->connectionListener->connectFinished)
            {
                _cond.wait(_lock);
            }
            _lock.unlock();

            this->currentSocket = this->socketIO->socket("/v1/"+this->socketIOnamespace);
            cout << "Current namespace: " << this->currentSocket->get_namespace() << endl;

/*
            // Delete old records sent
            $lastRecordIdSent = $self->getLastRecordIdSent();
            $self->db->exec("DELETE FROM write_operation WHERE id_write_operation <= $lastRecordIdSent");
*/

            cout << "DEVICE ID: " << this->deviceId << " DEVICE TOKEN: " << this->deviceToken << endl << "NAMESPACE: " << this->socketIOnamespace << endl;
            this->isConnected = true;
/*
            $this->startWriteQueueThread();
            $this->startHeartBeatTimer();
*/
            if(this->onConnectCallback != NULL) {
              (this->onConnectCallback)();
            }
/*
            // Launch the event loop of the socketIO
            $this->runLoop();
*/
  });

}

void DeviceClient::reconnect() {
   this->connect(this->host);
}

void DeviceClient::disconnect() {
   if(this->isConnected) {
     this->currentSocket->off_all();
     this->currentSocket->off_error();
     this->currentSocket->close();
     this->socketIO->sync_close();
     this->socketIO->clear_con_listeners();
     this->isConnected = false;

     if(this->onDisconnectCallback != NULL) {
       (this->onDisconnectCallback)();
     }
   }
}

rxcpp::observable<int> DeviceClient::connectToServer(const string& hostname, unsigned int apiPort, unsigned int webSocketPort, const string& deviceToken, const string& apiKey, const string& deviceKey) {

        static rxcpp::observable<int> observable = rxcpp::observable<>::create<int>(
                [this, &deviceToken, &hostname, apiPort, &apiKey, &deviceKey](rxcpp::subscriber<int> observer){
                try {
                        cout << "Connecting to server..." << endl;
                        string localDeviceToken("");

                        if(deviceToken.empty()) {
                                device_token_t* deviceTokenData = this->getDeviceTokenWithAPIKeyAndDeviceKey(hostname, 3001, apiKey, deviceKey);
                                if(deviceTokenData != NULL) {
                                        localDeviceToken = deviceTokenData->token;
                                        this->deviceId = deviceTokenData->device_id;
                                }
                        } else {
                                localDeviceToken = deviceToken;
                        }

                        this->deviceToken = localDeviceToken;
                        curlpp::Cleanup cleaner;
                        curlpp::Easy request;
                        string scheme("http://");
                        string endpoint("/v1/request-namespace");

                        curlpp::options::Url serverUrl(scheme+hostname+endpoint);
                        curlpp::options::Port serverPort(apiPort);

                        list<string> headersList;
                        headersList.push_back("Accept: application/json");
                        headersList.push_back("Authorization: "+localDeviceToken);
                        curlpp::options::HttpHeader headers(headersList);

                        ostringstream os;
                        curlpp::options::WriteStream ws(&os);
                        request.setOpt(ws);
                        request.setOpt(headers);
                        request.setOpt(serverUrl);
                        request.setOpt(serverPort);
                        request.perform();

                        string jsonResponse = os.str();
                        cout << "RESPONSE: " << jsonResponse << endl;
                        Document d;
                        d.Parse(jsonResponse.c_str());
                        Value& s = d["namespace"];
                        this->socketIOnamespace = s.GetString();

                        observer.on_completed();
                } catch( curlpp::RuntimeError &e ) {
                        cout << e.what() << endl;
                        observer.on_error(std::current_exception());
                } catch( curlpp::LogicError &e ) {
                        cout << e.what() << endl;
                        observer.on_error(std::current_exception());
                }

        });

        return observable;
}

int main(int, char **)
{
        string hostname("localhost"); //"maduixa.lafruitera.com");
        string apiKey("app1234");
        string deviceKey("key1234");

        DeviceClient *client = new DeviceClient(apiKey, deviceKey, hostname);

        client->onConnect([&client]() {
          cout << "Connected!" << endl;
          usleep(3000000);
          client->disconnect();
        });

        client->onDisconnect([]() {
          cout << "Disconnected!" << endl;
        });

        client->onConnectError([](const std::exception& ex) {
          cout << "ERROR when connecting: " << ex.what() << endl;
        });

        client->connect();
usleep(10000000);
        return 0;
}
