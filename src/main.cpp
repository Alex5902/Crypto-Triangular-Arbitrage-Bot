#include <iostream>
#include <thread>
#include <atomic>
#include <fstream>
#include <nlohmann/json.hpp>

#include "core/wallet.hpp"
#include "exchange/i_exchange_executor.hpp"
#include "exchange/binance_dry_executor.hpp"
#include "exchange/binance_real_executor.hpp"
#include "exchange/binance_account_sync.hpp"
#include "exchange/key_encryptor.hpp"

#include "engine/simulator.hpp"
#include "engine/triangle_scanner.hpp"
#include "core/orderbook.hpp"

// A small helper to load JSON config safely
static nlohmann::json loadConfig(const std::string& path) {
    nlohmann::json j;
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "[CONFIG] Could not open " << path
                  << ", using defaults.\n";
        return nlohmann::json::object();
    }
    try {
        f >> j;
    } catch(...) {
        std::cerr << "[CONFIG] Parse error in " << path
                  << ", using defaults.\n";
    }
    return j;
}

// Simple TUI function: prints a “dashboard” with trades so far
static void printDashboard(const Simulator& sim) {
    std::cout << "\n======== DASHBOARD ========\n";
    std::cout << " Total trades so far:   " << sim.getTotalTrades() << "\n";
    std::cout << " Cumulative profit (USDT est): " << sim.getCumulativeProfit() << "\n";
    std::cout << "==========================\n";
}

int main(int argc, char** argv) {
    // 0) Check CLI args for --live
    bool useLiveTrades = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--live") {
            useLiveTrades = true;
        }
    }

    // 1) Load config
    nlohmann::json cfg = loadConfig("config/bot_config.json");

    double fee          = cfg.value("fee", 0.001);
    double slippage     = cfg.value("slippage", 0.005);
    double maxFraction  = cfg.value("maxFractionPerTrade", 0.5); 
    double minFill      = cfg.value("minFill", 0.2);
    double threshold    = cfg.value("threshold", 0.0);
    bool useTestnet     = cfg.value("useTestnet", false);
    double minProfit    = cfg.value("minProfitUSDT", 0.5);
    std::string pairsFile = cfg.value("pairsFile", "config/pairs.json");

    // 1b) Load wallet from config
    Wallet wallet;
    if (cfg.contains("walletInit") && cfg["walletInit"].is_object()) {
        for (auto it = cfg["walletInit"].begin(); it != cfg["walletInit"].end(); ++it) {
            std::string asset = it.key();
            double amount     = it.value().get<double>();
            wallet.setBalance(asset, amount);
        }
    } else {
        // fallback
        wallet.setBalance("BTC", 0.02);
        wallet.setBalance("ETH", 0.5);
        wallet.setBalance("USDT", 200.0);
    }

    std::cout << "[CONFIG] fee=" << fee
              << " slip=" << slippage
              << " maxFraction=" << maxFraction
              << " minFill=" << minFill
              << " threshold=" << threshold
              << " useTestnet=" << (useTestnet?"true":"false")
              << " pairsFile=" << pairsFile << "\n";

    // 2) Decide executor
    IExchangeExecutor* executor = nullptr;
    std::atomic<bool> keepSyncing(true);
    std::thread syncThread;

    if (!useTestnet) {
        // DRY mode => no real trades
        auto* dryExec = new BinanceDryExecutor(1.0, 150, 28000.0);
        executor = dryExec;
        std::cout << "[EXECUTOR] Using DRY RUN mode.\n";
    } else {
        // We use testnet with encrypted keys
        std::string passphrase;
        {
            std::ifstream pf("config/passphrase.txt");
            if(!pf.is_open()) {
                std::cerr << "[EXECUTOR] Could not open config/passphrase.txt!\n";
                return 1;
            }
            std::getline(pf, passphrase);
            if(passphrase.empty()) {
                std::cerr << "[EXECUTOR] passphrase is empty.\n";
                return 1;
            }
        }

        std::string encryptedKeys;
        {
            std::ifstream kf("config/keys.enc");
            if(!kf.is_open()) {
                std::cerr << "[EXECUTOR] Could not open config/keys.enc\n";
                return 1;
            }
            std::stringstream buffer;
            buffer << kf.rdbuf();
            encryptedKeys = buffer.str();
        }

        std::string decrypted;
        try {
            decrypted = KeyEncryptor::decryptData(passphrase, encryptedKeys);
        } catch(...) {
            std::cerr << "[EXECUTOR] Decrypted text not valid!\n";
            return 1;
        }

        nlohmann::json keyJson;
        try {
            keyJson = nlohmann::json::parse(decrypted);
        } catch(...) {
            std::cerr << "[EXECUTOR] Decrypted text not valid JSON!\n";
            return 1;
        }

        if(!keyJson.contains("apiKey") || !keyJson.contains("secretKey")) {
            std::cerr << "[EXECUTOR] Missing fields in decrypted keys!\n";
            return 1;
        }

        std::string apiKey = keyJson["apiKey"].get<std::string>();
        std::string secretKey = keyJson["secretKey"].get<std::string>();

        std::string baseUrl = "https://testnet.binance.vision";
        auto* realExec = new BinanceRealExecutor(apiKey, secretKey, baseUrl);
        executor = realExec;

        // spawn a wallet sync thread
        startWalletSyncThread(&wallet, apiKey, secretKey, baseUrl, &keepSyncing, syncThread);
        std::cout << "[EXECUTOR] Using REAL BINANCE TESTNET mode (encrypted keys).\n";
    }

    // 3) Create simulator
    Simulator sim("sim_log.csv", fee, slippage,
                  maxFraction, // interpret as fraction of free balance
                  minFill,
                  &wallet, executor, minProfit);

    // set live mode if user passed --live
    if (useLiveTrades) {
        std::cout << "[MAIN] Live execution mode is ENABLED.\n";
        sim.setLiveMode(true);
    } else {
        std::cout << "[MAIN] Live execution mode is OFF (simulation only).\n";
    }

    // 4) Create scanner + orderbook
    TriangleScanner scanner;
    OrderBookManager obm(&scanner);
    scanner.setOrderBookManager(&obm);

    // 5) pass simulator to scanner
    scanner.setSimulator(&sim);

    // 6) dynamic load from /exchangeInfo => BFS-based cycle detection
    // If that fails, fallback to file
    if (!scanner.loadTrianglesFromBinanceExchangeInfo()) {
        std::cerr << "[MAIN] Could not load dynamic triangles => fallback to file: " << pairsFile << "\n";
        scanner.loadTrianglesFromFile(pairsFile);
    }
    scanner.setMinProfitThreshold(threshold);

    // Now that all symbols are known (from BFS or file),
    // we open a single combined WebSocket for them:
    obm.startCombinedWebSocket();

    std::cout << "[MAIN] Bot running. Press Ctrl+C to quit.\n";

    // 7) main loop
    // Optionally: we could re-score all triangles here every 30s, then trade top N
    // For now, we just do a TUI print:
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        wallet.printAll();
        printDashboard(sim);

        // Example of re-scoring:
        //   1) scanner.rescoreAllTrianglesConcurrently(...);
        //   2) pick top X from scanner, or do an external approach
    }

    // cleanup on exit
    keepSyncing.store(false);
    if (syncThread.joinable()) {
        syncThread.join();
    }
    delete executor;

    return 0;
}
