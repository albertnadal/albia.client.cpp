#include <string>
#include <vector>
#include <sstream>
#include <iostream>

#include <curlpp/cURLpp.hpp>
#include <curlpp/Easy.hpp>
#include <curlpp/Options.hpp>

#include "rapidjson/document.h"
#include "rxcpp/rx.hpp"
#include "base64.h"

using namespace rapidjson;
using namespace std;

const vector<string> explode(const string& s, const char& c)
{
   string buff{""};
   vector<string> v;

   for(auto n:s) {
      if(n != c) buff+=n; else
      if(n == c && buff != "") { v.push_back(buff); buff = ""; }
   }
   if(buff != "") v.push_back(buff);

   return v;
}

struct device_token_t {
  string token;
  unsigned int device_id;
};

device_token_t* getDeviceTokenWithAPIKeyAndDeviceKey(const string& hostname, unsigned int apiPort, const string& apiKey, const string& deviceKey) {

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
      vector<string> tokenVector = explode(decodedToken, ';');
      deviceToken->device_id = stoi(tokenVector[0]);

      return deviceToken;

   } catch( curlpp::RuntimeError &e ){
      cout << e.what() << endl;
   } catch( curlpp::LogicError &e ) {
      cout << e.what() << endl;
   }

   return NULL;
}

struct connect_device_data_t {
  device_token_t* device_token_data;
  string socket_io_namespace;
};

rxcpp::observable<connect_device_data_t*> connectToServer(const string& hostname, unsigned int apiPort, unsigned int webSocketPort, const string& deviceToken, const string& apiKey, const string& deviceKey) {

    static rxcpp::observable<connect_device_data_t*> observable = rxcpp::observable<>::create<connect_device_data_t*>(
        [&deviceToken, &hostname, apiPort, &apiKey, &deviceKey](rxcpp::subscriber<connect_device_data_t*> observer){
            try {

               cout << "Connecting to server..." << endl;

               connect_device_data_t* connectDeviceData = new connect_device_data_t;
               connectDeviceData->device_token_data = NULL;

               string localDeviceToken("");

               if(deviceToken.empty()) {
                  device_token_t* deviceTokenData = getDeviceTokenWithAPIKeyAndDeviceKey(hostname, 3001, apiKey, deviceKey);

                  if(deviceTokenData != NULL) {
                     localDeviceToken = deviceTokenData->token;
                     connectDeviceData->device_token_data = deviceTokenData;
                  }

               } else {
                  localDeviceToken = deviceToken;
               }

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
               string socketIOnamespace = s.GetString();
               connectDeviceData->socket_io_namespace = socketIOnamespace;

               observer.on_next(connectDeviceData);
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
   string hostname("maduixa.lafruitera.com");
   string apiKey("app1234");
   string deviceKey("key1234");
   string deviceToken("");
   string socketIOnamespace("");
   unsigned int deviceId = 0;

   rxcpp::observable<connect_device_data_t*> observable = connectToServer(hostname, 3001, 3000, deviceToken, apiKey, deviceKey);

   observable.subscribe(
            [&deviceToken, &socketIOnamespace, &deviceId](connect_device_data_t* connectDeviceData) {
                deviceToken = connectDeviceData->device_token_data->token;
                deviceId = connectDeviceData->device_token_data->device_id;
                delete connectDeviceData->device_token_data;
                socketIOnamespace = connectDeviceData->socket_io_namespace;
                delete connectDeviceData;
                cout << "DEVICE TOKEN: " << deviceToken << endl << "NAMESPACE: " << socketIOnamespace << endl;
            },
            [](std::exception_ptr ep){
                try {std::rethrow_exception(ep);}
                catch (const std::exception& ex) {
                    printf("OnError: %s\n", ex.what());
                }
            },
            [](){

            });


   return 0;
}
