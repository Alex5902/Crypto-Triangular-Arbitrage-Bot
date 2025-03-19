#include "core/wallet.hpp"
#include "exchange/i_exchange_executor.hpp"
#include "exchange/binance_dry_executor.hpp"
#include "engine/simulator.hpp"
#include "engine/triangle_scanner.hpp"
#include "core/orderbook.hpp"

int main() {
    // create the wallet
    Wallet wallet;
    wallet.setBalance("BTC", 0.02);
    wallet.setBalance("ETH", 0.5);
    wallet.setBalance("USDT", 200.0);

    // create the dry executor
    BinanceDryExecutor dryExec(/*fillRatio=*/1.0, /*latency=*/150, /*mockPrice=*/28000.0);

    // create simulator
    Simulator sim("sim_log.csv",
                  /*fee=*/0.001, /*slippage=*/0.005,
                  /*volumeLimit=*/1.0, /*minFill=*/0.2,
                  &wallet, &dryExec);

    // create scanner, obm, etc.
    TriangleScanner scanner;
    OrderBookManager obm(&scanner);
    scanner.setOrderBookManager(&obm);

    // pass simulator to scanner
    scanner.setSimulator(&sim);

    // load pairs
    scanner.loadTrianglesFromFile("config/pairs.json");
    scanner.setMinProfitThreshold(0.0);

    // run
    while(true) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        wallet.printAll();
    }

    return 0;
}
