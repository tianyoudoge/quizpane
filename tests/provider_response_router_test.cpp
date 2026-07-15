#include "quizpane/provider_response_router.hpp"
#include <QJsonArray>

int main() {
    using namespace quizpane;
    const auto catalog = routeProviderResponse({{"id", "catalog-list"},
        {"result", QJsonObject{{"nodes", QJsonArray{}}}}});
    if (catalog.route != ProviderRoute::Catalog || catalog.failed) return 1;
    const auto save = routeProviderResponse({{"id", "save-2-1"},
        {"error", QJsonObject{{"code", -1}, {"message", "failed"}}}});
    if (save.route != ProviderRoute::SaveAnswer || !save.failed) return 2;
    return 0;
}
