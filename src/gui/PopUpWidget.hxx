//============================================================================
//
//   SSSS    tt          lll  lll
//  SS  SS   tt           ll   ll
//  SS     tttttt  eeee   ll   ll   aaaa
//   SSSS    tt   ee  ee  ll   ll      aa
//      SS   tt   eeeeee  ll   ll   aaaaa  --  "An Atari 2600 VCS Emulator"
//  SS  SS   tt   ee      ll   ll  aa  aa
//   SSSS     ttt  eeeee llll llll  aaaaa
//
// Copyright (c) 1995-2018 by Bradford W. Mott, Stephen Anthony
// and the Stella Team
//
// See the file "License.txt" for information on usage and redistribution of
// this file, and for a DISCLAIMER OF ALL WARRANTIES.
//============================================================================

#ifndef POPUP_WIDGET_HXX
#define POPUP_WIDGET_HXX

class GUIObject;
class ContextMenu;

#include "bspf.hxx"
#include "Variant.hxx"
#include "Command.hxx"
#include "Widget.hxx"

/**
 * Popup or dropdown widget which, when clicked, "pop up" a list of items and
 * lets the user pick on of them.
 *
 * Implementation wise, when the user selects an item, then a kPopUpItemSelectedCmd
 * is broadcast, with data being equal to the tag value of the selected entry.
 */
class PopUpWidget : public Widget, public CommandSender
{
  public:
    PopUpWidget(GuiObject* boss, const GUI::Font& font,
                int x, int y, int w, int h, const VariantList& items,
                const string& label, int labelWidth = 0, int cmd = 0);
    virtual ~PopUpWidget() = default;

    int getTop() const override { return _y + 1; }
    int getBottom() const override { return _y + 1 + getHeight(); }

    /** Add the given items to the widget. */
    void addItems(const VariantList& items);

    /** Various selection methods passed directly to the underlying menu
        See ContextMenu.hxx for more information. */
    void setSelected(const Variant& tag,
                     const Variant& def = EmptyVariant);
    void setSelectedIndex(int idx, bool changed = false);
    void setSelectedMax(bool changed = false);
    void clearSelection();

    int getSelected() const;
    const string& getSelectedName() const;
    const Variant& getSelectedTag() const;

    bool wantsFocus() const override { return true; }

  protected:
    void handleMouseDown(int x, int y, MouseButton b, int clickCount) override;
    void handleMouseWheel(int x, int y, int direction) override;
    void handleMouseEntered() override;
    void handleMouseLeft() override;
    bool handleEvent(Event::Type e) override;
    void handleCommand(CommandSender* sender, int cmd, int data, int id) override;
    void drawWidget(bool hilite) override;

  private:
    unique_ptr<ContextMenu> myMenu;
    int myArrowsY;
    int myTextY;

    string _label;
    int    _labelWidth;
    bool   _changed;

  private:
    // Following constructors and assignment operators not supported
    PopUpWidget() = delete;
    PopUpWidget(const PopUpWidget&) = delete;
    PopUpWidget(PopUpWidget&&) = delete;
    PopUpWidget& operator=(const PopUpWidget&) = delete;
    PopUpWidget& operator=(PopUpWidget&&) = delete;
};

#endif
