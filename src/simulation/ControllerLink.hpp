#pragma once
#include <memory>
namespace ui::cli
{
    class Parser;
}
namespace simulation
{
    class ControllerLink final
    {
      public:
        ControllerLink();
        ~ControllerLink();
        ControllerLink(const ControllerLink&) = delete;
        ControllerLink& operator=(const ControllerLink&) = delete;

      private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
    void setupCommands(ui::cli::Parser& parser);
}
