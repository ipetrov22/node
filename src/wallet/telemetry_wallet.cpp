#include "wallet.h"
#include "telemetry_wallet.h"

#include <stdarg.h>
#include <queue>
#include <iostream>
#include <fstream>

#include "validation.h"
#include <util/strencodings.h>
#include <zlib.h>
#include "openssl/sha.h"

#include "base58.h"
#include "checkpoints.h"
#include "chain.h"
#include "wallet/coincontrol.h"
#include <wallet/wallet.h>
#include "consensus/consensus.h"
#include "consensus/validation.h"
#include "key.h"
#include <key_io.h>
#include "keystore.h"
#include "validation.h"
#include "script/script.h"
#include "script/sign.h"
#include <script/standard.h>
#include "scheduler.h"

#include "rpc/server.h"
#include "rpc/client.h"
#include <wallet/rpcwallet.h>
#include <univalue.h>


#define MAX_LOG_SIZE 100*1024*1024
//#define TELEMETRY_HOST "stats.locktrip.com"
//#define TELEMETRY_PORT 8080
#define TELEMETRY_HOST "68.183.76.56"
#define TELEMETRY_PORT 8181

bool telemetry_first = true;
std::string telemetry_data = "";

std::string TelemetryCompress(const std::string& str,int compressionlevel = Z_BEST_COMPRESSION) {
    z_stream zs;                        // z_stream is zlib's control structure
    memset(&zs, 0, sizeof(zs));

    if (deflateInit(&zs, compressionlevel) != Z_OK)
        throw(std::runtime_error("deflateInit failed while compressing."));

    zs.next_in = (Bytef*)str.data();
    zs.avail_in = str.size();           // set the z_stream's input

    int ret;
    char outbuffer[32768];
    std::string outstring;

    // retrieve the compressed bytes blockwise
    do {
        zs.next_out = reinterpret_cast<Bytef*>(outbuffer);
        zs.avail_out = sizeof(outbuffer);

        ret = deflate(&zs, Z_FINISH);

        if (outstring.size() < zs.total_out) {
            // append the block to the output string
            outstring.append(outbuffer,
                             zs.total_out - outstring.size());
        }
    } while (ret == Z_OK);

    deflateEnd(&zs);

    if (ret != Z_STREAM_END) {          // an error occurred that was not EOF
        std::ostringstream oss;
        oss << "Exception during zlib compression: (" << ret << ") " << zs.msg;
        throw(std::runtime_error(oss.str()));
    }

    return outstring;
}

std::vector<unsigned char> TelemetryHash(std::string str) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, str.c_str(), str.size());
    SHA256_Final(hash, &sha256);
    std::vector<unsigned char> ss;
    ss.assign(hash, hash+SHA256_DIGEST_LENGTH);
    return ss;
}

bool TelemetryGetKeys(CPubKey& pubKey, CKey& vchSecret) {
	std::shared_ptr<CWallet> pwallet = vpwallets.size() >= 1 ? vpwallets[0] : std::shared_ptr<CWallet>(nullptr);
	if(pwallet == nullptr)
		return false;

	CTxDestination tx_dest;
    pwallet->GetLabelDestination(tx_dest, "");
	std::string pubKeyStr = EncodeDestination(tx_dest);
	std::vector<unsigned char> pubKeyData (pubKeyStr.begin(), pubKeyStr.end());
	CPubKey tempPubKey(pubKeyData);
	pubKey = tempPubKey;

	LOCK2(cs_main, pwallet->cs_wallet);
	EnsureWalletIsUnlocked(pwallet.get());

	CTxDestination dest = DecodeDestination(pubKey.GetID().ToString());
	if (!IsValidDestination(dest)) {
		return false;
	}
	auto KeyID = GetKeyForDestination(*pwallet, dest);
	if (KeyID.IsNull()) {
		return false;
	}

	return true;
}

void TelemetryUpload() {
	try {
		if(telemetry_first) {
			telemetry_data = ReadTelemetryLogsSync();
		}
		std::string data = TelemetryQGet();
		if(telemetry_data.length() > MAX_LOG_SIZE) { //max 100 MB buffered logs
			telemetry_data = "";
			SaveTelemetryLogsSync("", false);
		}
		telemetry_data += data;
		if(telemetry_data == "")
			return;

		CPubKey pubKey;
		CKey vchSecret;
		if(!TelemetryGetKeys(pubKey, vchSecret))
			return;

		std::string encoded = EncodeBase64(TelemetryCompress(telemetry_data));
		std::vector<unsigned char> vec = TelemetryHash(encoded);
		uint256 hash(vec);
		std::vector<unsigned char> vchSig;
		if (!vchSecret.SignCompact(hash, vchSig)) {
			return;
		}
		std::string sign = EncodeBase64(&vchSig[1], vchSig.size() - 1);

		std::vector<unsigned char> v = pubKey.getvch();
		std::string pkey(v.begin(), v.end());
		std::string pk = EncodeBase64(pkey);
		CTxDestination wallet = DecodeDestination(pubKey.GetID().ToString());
		std::string msg ="{\"wallet\":,";// "{\"wallet\":\"" + wallet + "\",";
		msg += "\"pkey\":\"" + pk + "\",";
		msg += "\"data\":\"" + encoded + "\",";
		if(telemetry_first) {
			msg += "\"first\":1,";
		}
		msg += "\"hash\":\"" + sign + "\"}";

		hostent * record = gethostbyname(TELEMETRY_HOST);
		if(record == NULL) {
			return;
		}

		sockaddr_in sockaddr;
		sockaddr.sin_addr = *(in_addr*)(record->h_addr);
		sockaddr.sin_port = htons(TELEMETRY_PORT);
		sockaddr.sin_family = AF_INET;
		socklen_t len = sizeof(sockaddr_in);

		int hSocket = socket(AF_INET, SOCK_STREAM, 0);
		if (hSocket == -1) {
			return;
		}
	#ifdef SO_NOSIGPIPE
		int set = 1;
		// Different way of disabling SIGPIPE on BSD
		setsockopt(hSocket, SOL_SOCKET, SO_NOSIGPIPE, (void*)&set, sizeof(int));
	#endif
		if (connect(hSocket, (struct sockaddr*)&sockaddr, len) == SOCKET_ERROR) {
			return;
		}
		int ret = send(hSocket, msg.c_str(), msg.length(), 0);
		if(ret < 1) {
			close(hSocket);
			return;
		}
		telemetry_data = "";
		SaveTelemetryLogsSync("", false);

		std::string blacklist = "";
		if(telemetry_first) {
			telemetry_first = false;
			int count = 0;
			int result;
			char buffer[32768];
			do {
				memset(buffer, 0, 32768);
				result = recv(hSocket, buffer, 32768, 0);
				count += result;
				if (result < 1 || count > 32768*64) {
					close(hSocket);
					return;
				}
				blacklist.append(buffer);
			} while(buffer[result - 1] != '#');
			blacklist.erase(blacklist.end()-1);
			TelemetrySetBlacklisted(blacklist);
		}

		close(hSocket);
	} catch(...) {
		 std::cerr << "Encrypted wallet. Can't send telemetry data." << "\n";
	}
}

UniValue getpeerinfo(const JSONRPCRequest& request);
void DumpPeers() {
	/*JSONRPCRequest request;
	UniValue res = getpeerinfo(request);
	std::string str = res.write() + "\n";
	LogPrintf(str.c_str());*/
}

