// Released under the terms of the AGPLv3 or higher.
#ifndef slic3r_GUI_CPAMTracePanel_hpp_
#define slic3r_GUI_CPAMTracePanel_hpp_

#include <memory>
#include <wx/panel.h>

namespace Slic3r::GUI {

class CPAMTracePanel : public wxPanel
{
public:
    explicit CPAMTracePanel(wxWindow *parent);
    ~CPAMTracePanel() override;
    void refresh();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Slic3r::GUI

#endif
