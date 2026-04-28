#pragma once

namespace DA {
  namespace utils {
    template <typename Derived> class Singleton {
    public:
      static Derived& GetInstance() {
        static Derived instance;
        return instance;
      }

      Singleton(Derived&&) = delete;
      Singleton(const Derived&) = delete;
      void operator=(const Derived&) = delete;

    protected:
      Singleton() = default;
      virtual ~Singleton() = default;
    };
  }  // namespace utils
}  // namespace DA