#include <cstdlib>

#include "quizpane/attempt.hpp"

int main() {
    quizpane::Attempt attempt;
    attempt.id = "attempt-1";
    attempt.providerId = "org.quizpane.demo";
    attempt.questionIds = {"q1", "q2"};
    attempt.requestedCount = 2;
    attempt.actualCount = 2;
    attempt.state = quizpane::AttemptState::Answering;
    const auto restored = quizpane::Attempt::fromJson(attempt.toJson());
    return restored.id == attempt.id && restored.questionIds == attempt.questionIds
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
