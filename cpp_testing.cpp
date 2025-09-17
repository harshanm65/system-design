#include <utility>
#include <iostream>

using namespace std;

int main() {
    int a = 3;
    int b;
    b = std::exchange(a, 5);
    cout << a << " " << b << endl;
}
