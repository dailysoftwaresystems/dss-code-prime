// FC17.5 (D-CSUBSET-AUTO-TYPE-INFERENCE): the ★C1 presence-gate pins. All
// four specifier-led implicit-int shapes PARSE into the headless
// autoInferredVarDecl rule (its grammar cannot know which specifier is the
// inference marker) and MUST stay errors — C23 removed implicit int, and
// silently adopting the initializer's type would compile C89 forms as if
// they were inference declarations. Each fires S_AutoInferenceInvalid
// (unsuppressable) at its own declaration.
int main(void) {
    static x = 5;
    register y = 2;
    alignas(4) z = 9;
    [[maybe_unused]] w = 3;
    return x + y + z + w;
}
