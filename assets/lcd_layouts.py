# SPDX-License-Identifier: GPL-3.0-only
"""Adapt the pinned upstream layouts while preserving their widget bindings."""
import xml.etree.ElementTree as ET


def layout(source, relative):
    # Borealis treats brls: as part of the widget name, without an XML namespace.
    return ET.fromstring((source / relative).read_text(encoding='utf-8').replace('brls:', 'brls-'))


def save(root, target, relative):
    path = target / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    ET.indent(root, space='    ')
    path.write_text(ET.tostring(root, encoding='unicode').replace('brls-', 'brls:') + '\n', encoding='utf-8')


def widget(root, name):
    return next(node for node in root.iter() if node.get('id') == name)


def fit_tabs(root, height):
    for frame in root.iter('AutoTabFrame'):
        frame.set('tabHeight', str(height))
        for tab in frame.findall('Tab'):
            tab.set('fontSize', '17')


def fit_dynamic_feed(root, *, lcd):
    root.remove(widget(root, 'dynamic/up/recyclingGrid'))
    tabs = widget(root, 'dynamic/tab/frame')
    tabs.attrib.pop('disableNavigationRight', None)
    tabs.attrib.update(tabHeight='36' if lcd else '56', paddingBottom='4' if lcd else '8')
    for tab in tabs:
        tab.set('fontSize', '19' if lcd else '22')
    video = widget(tabs, 'dynamic/videoList')
    video.attrib.update(preFetchLine='1' if lcd else '2', itemHeight='175' if lcd else '250')
    articles = widget(tabs, 'dynamic/articleList')
    articles.attrib.update(paddingLeft='8' if lcd else '48', paddingRight='8' if lcd else '48',
                           preFetchLine='1')


def generate(source, target):
    for name in ('home_hots_all', 'home_live', 'mine_tab', 'mine_history', 'mine_later',
                 'search_history', 'search_hots', 'search_video'):
        path = 'fragment/' + name + '.xml'
        root = layout(source, path)
        root.attrib.pop('positionTop', None)
        root.attrib.pop('marginTop', None)
        fit_tabs(root, 34)
        for grid in root.iter('RecyclingGrid'):
            grid.attrib.update(paddingLeft='6', paddingRight='6')
        if name in ('home_hots_all', 'home_live', 'mine_history', 'mine_later', 'search_video'):
            root.find('RecyclingGrid').set('itemHeight', '175')
        if name == 'home_live':
            root.remove(root.find('brls-Box'))
        if name == 'mine_tab':
            header = widget(root, 'user_home/goto_userspace')
            header.attrib.update(alignItems='center', height='61', marginTop='4', marginRight='6', marginLeft='4')
            widget(root, 'mine/image/avatar').attrib.update(width='38', height='38', margin='4', shrink='0', scalingType='fit')
            for label in header.iter('brls-Label'):
                label.set('marginTop', '0')
                label.set('fontSize', '17')
            for divider in header.iter('brls-Rectangle'):
                divider.set('marginLeft', '2')
                divider.set('marginRight', '2')
            identity, counts = header[1], header[2]
            identity.attrib.update(width='104', marginLeft='3', justifyContent='center')
            for label in identity:
                label.attrib.update(grow='0', singleLine='true', marginBottom='0', width='100%')
            widget(header, 'mine/label/sign').set('fontSize', '14')
            counts.attrib.update(justifyContent='flexStart', marginRight='2')
            for pair in counts.findall('brls-Box'):
                pair.attrib.update(width='58', grow='0')
            tabs = widget(root, 'mine/tab/frame')
            for tab in tabs.findall('Tab'):
                if tab[0].tag not in ('MineHistory', 'MineLater'):
                    tabs.remove(tab)
        if name in ('search_history', 'search_hots'):
            root.find('RecyclingGrid').attrib.update(itemHeight='28', itemSpace='2', paddingLeft='1', paddingRight='2')
        save(root, target, path)

    path = 'fragment/dynamic_tab.xml'
    root = layout(source, path)
    fit_dynamic_feed(root, lcd=True)
    save(root, target, path)

    path = 'views/hots_card.xml'
    root = layout(source, path)
    widget(root, 'hots/order').attrib.update(width='29', height='28', fontSize='17', shrink='0')
    widget(root, 'hots/content').attrib.update(height='28', grow='1', fontSize='18', singleLine='true')
    widget(root, 'hots/icon').set('height', '16')
    save(root, target, path)

    path = 'views/user_info.xml'
    root = layout(source, path)
    avatar = widget(root, 'userinfo/avatar')
    avatar.attrib.update(width='28', height='28', marginLeft='2', borderThickness='1')
    avatar.attrib.pop('maxHeight')
    root[1].attrib.update(marginLeft='3', justifyContent='center')
    for label in root[1]:
        label.attrib.update(grow='0', width='100%', marginTop='0', marginBottom='0', fontSize='15')
    widget(root, 'userinfo/box/hint').attrib.update(width='60', height='25', marginRight='2')
    widget(root, 'userinfo/svg/hint').attrib.update(width='12', height='12', marginLeft='1', marginRight='2')
    widget(root, 'userinfo/label/hint').set('fontSize', '14')
    save(root, target, path)

    path = 'activity/setting_activity.xml'
    root = layout(source, path)
    root.set('footerHidden', 'true')
    widget(root, 'setting/tabFrame').set('sidebarWidth', '54')
    for tab in root.iter('Tab'):
        title = tab.get('label')
        tab.attrib.update(label='', style='accent')
        scroll = tab[0]
        scroll.attrib.update(height='auto', marginTop='0')
        if scroll[0].get('xml') == '@res/xml/fragment/settings_about.xml':
            # Borealis resolves inline XML includes from the base resource directory.
            scroll[0].set('xml', '@res/lcd/xml/fragment/settings_about.xml')
        if scroll[0].tag == 'brls-Box':
            scroll[0].attrib.update(paddingTop='2', paddingBottom='6')
        pane = ET.Element('brls-Box', axis='column', grow='1')
        ET.SubElement(pane, 'brls-Label', text=title, height='31', marginTop='4', marginLeft='8',
                      fontSize='21', verticalAlign='center', singleLine='true')
        tab[:] = [pane]
        pane.append(scroll)
    save(root, target, path)

    path = 'fragment/settings_about.xml'
    root = layout(source, path)
    root.set('axis', 'column')
    credits, repository = root
    repository.attrib = {'alignItems': 'center', 'marginTop': '6'}
    repository[0].attrib.update(width='80', height='80')
    brand = credits[0]
    brand.attrib.update(height='36', alignItems='center', marginBottom='4')
    brand[0].attrib.update(width='100', fontSize='28', singleLine='true', shrink='0')
    brand[1].attrib.update(fontSize='16', singleLine='true')
    for header in credits.iter('brls-Header'):
        header.set('marginBottom', '4')
    for node in credits.iter():
        if node.get('width', '').startswith('@style/'):
            node.set('width', 'auto')
    save(root, target, path)

    path = 'activity/player_activity.xml'
    root = layout(source, path)
    root.set('footerHidden', 'true')
    left = widget(root, 'video_detail_left_box')
    left.set('margin', '4')
    info = widget(root, 'video_detail_info_box')
    info.set('height', '120')
    title, byline, metadata = info
    title.attrib.update(height='30', marginTop='0')
    widget(title, 'video/title').attrib.update(fontSize='20', marginLeft='2')
    byline.attrib.update(height='67', axis='column')
    byline[0].attrib.update(width='100%', height='31')
    widget(byline, 'video_author').attrib.update(marginLeft='0', height='100%')
    actions = byline[1]
    actions.attrib.update(height='29', width='100%', marginLeft='0', marginBottom='0', marginTop='7',
                          paddingLeft='4', paddingRight='4')
    for action in actions:
        action.attrib.update(axis='row', height='28', width='25%', padding='0', justifyContent='center')
        action[0].attrib.update(width='18', height='18', positionTop='0')
        action[1].attrib.update(positionType='relative', positionBottom='0', width='auto',
                                marginLeft='4', fontSize='15', singleLine='true')
    metadata.attrib.update(marginLeft='2', marginTop='2', height='20')
    # The information dialog also contains the date and BV identifier.
    for label_id in ('video/time', 'video/bvid'):
        label = widget(metadata, label_id)
        label.set('visibility', 'gone')
    children = list(metadata)
    children[children.index(widget(metadata, 'video/time')) - 1].set('visibility', 'gone')
    fit_tabs(root, 36)
    save(root, target, path)

    path = 'views/video_view.xml'
    root = layout(source, path)
    top = widget(root, 'video/osd/top/box')
    top.set('height', '46')
    widget(top, 'video/osd/title').attrib.update(fontSize='18', height='26', marginLeft='6', marginRight='50')
    for name in ('cast', 'setting'):
        button = widget(top, 'video/osd/' + name)
        button.attrib.update(width='32', height='32', positionTop='2', positionRight='4')
        button[0].attrib.update(width='22', height='22')
    bottom = widget(root, 'video/osd/bottom/box')
    bottom.set('height', '66')
    play, timeline = bottom
    play.attrib.update(width='32', height='32', marginLeft='4', marginBottom='0')
    play[0].attrib.update(width='30', height='30')
    timeline.set('height', '64')
    slider, controls = timeline[:2]
    slider.attrib.update(height='22', marginTop='0')
    controls.set('height', '40')
    for label in controls.findall('brls-Label'):
        label.set('fontSize', '15')
        if 'width' in label.attrib:
            label.set('width', '46')
    buttons = widget(controls, 'video/osd/icon/box')
    buttons.set('positionRight', '2')
    for button in buttons:
        button.attrib.update(height='32', marginLeft='2', marginRight='2')
        if button.get('width'):
            button.set('width', '26')
        for icon in button.iter('SVGImage'):
            icon.attrib.update(width='20', height='20')
        for label in button.iter('brls-Label'):
            label.set('fontSize', '15')
    save(root, target, path)

    path = 'views/video_profile.xml'
    root = layout(source, path)
    root.set('width', '348')
    # Fit the essential playback counters on one screen. Keep all bound labels.
    show = {'profile/video/res', 'profile/video/codec', 'profile/video/hwdec', 'profile/video/drop',
            'profile/video/avsync', 'profile/file/cache', 'profile/audio/codec'}
    for row in root:
        if row.tag == 'brls-Box':
            fields = [n for n in row if n.get('id') in show]
            if not fields:
                row.set('visibility', 'gone')
            else:
                row.set('height', '20')
                for label in row:
                    label.set('fontSize', '16')
    save(root, target, path)

    path = 'fragment/inbox_view.xml'
    root = layout(source, path)
    root.find('brls-AppletFrame').set('width', '96%')
    fit_tabs(root, 38)
    save(root, target, path)

    # Preserve the close button bindings used by modal code. Esc handles dismissal.
    path = 'views/button_close.xml'
    root = layout(source, path)
    root.attrib.update(visibility='gone', focusable='false')
    save(root, target, path)

    fit_screens(source, target)


def fit_screens(source, target):
    """Apply screen insets once, then fit lists and dialogs inside their actual parent."""
    for original in sorted(source.glob('*/*.xml')):
        path = original.relative_to(source)
        name = path.stem
        root = layout(target if (target / path).exists() else source, path)
        before = ET.tostring(root)

        if path.parent.name == 'activity' and name not in ('main', 'video_activity', 'search_activity',
                                                            'search_activity_tv'):
            root[0].set('padding', '8')
        for grid in root.iter('RecyclingGrid'):
            if name in ('search_hots', 'search_history'):
                continue
            grid.attrib.update(paddingTop='6', paddingBottom='6', paddingLeft='8', paddingRight='8')
            if grid.get('flowMode') != 'true' and grid.get('itemSpace') != '-1':
                grid.set('itemSpace', '8')
            if grid.get('spanCount', '').startswith('@style/wiliwili/grid/span/') and grid.get('itemHeight'):
                grid.set('itemHeight', '192' if 'bangumi' in name or name == 'pgc_index_activity' else '175')

        if name == 'mine_tab':
            root.set('marginLeft', '2')
            header = widget(root, 'user_home/goto_userspace')
            header.attrib.update(height='52', marginTop='0', marginBottom='5')
            header[1].set('width', '114')
            for divider in header.iter('brls-Rectangle'):
                divider.set('marginTop', '0')
            widget(root, 'mine/tab/frame').attrib.update(tabHeight='36', marginBottom='0')
        elif name in ('video_card', 'video_card_history', 'video_card_live', 'video_card_pgc'):
            # Reserve explicit rows so even a two-line title cannot displace the byline.
            cover, caption = root
            cover.set('height', '55%' if name == 'video_card' else '60%')
            caption.set('margin', '4')
            for badge in caption[2:]:
                badge.attrib.update(height='16', marginTop='1')
            if name == 'video_card_history':
                count = widget(cover, 'video/card/label/count')
                count.attrib.update(grow='1', shrink='1', singleLine='true', fontSize='14')
                widget(cover, 'video/card/label/duration').attrib.update(positionType='relative', shrink='0', fontSize='14')
        elif name == 'video_card_related':
            widget(root, 'video/card/picture').set('width', '100%')
            root[1].attrib.update(marginTop='2', marginLeft='5')
            widget(root, 'video/card/label/title').attrib.update(fontSize='16', height='38', lineHeight='1.12')
            widget(root, 'video/card/label/username').attrib.update(fontSize='14', shrink='1')
        elif name == 'inbox_view':
            root.set('padding', '8')
            root.find('brls-AppletFrame').set('width', '100%')
            widget(root, 'inbox/tab/frame').set('tabHeight', '40')
            widget(root, 'inbox/chatList').set('itemHeight', '52')
        elif name == 'inbox_chat':
            root.set('padding', '6')
            widget(root, 'inbox/talker').attrib.update(marginTop='3', marginBottom='6', marginLeft='8',
                                                       fontSize='19', singleLine='true')
            widget(root, 'inbox/reply/hint').attrib.update(marginTop='5', marginBottom='5', marginLeft='8', marginRight='8')
        elif name == 'feed_card':
            root.set('padding', '8')
            root[0].set('marginRight', '8')
            widget(root, 'feed/avatar').set('width', '34')
            widget(root, 'feed/content_box').set('marginRight', '8')
            widget(root, 'feed/label/author').attrib.update(fontSize='17', singleLine='true')
            widget(root, 'feed/card/pic_box').attrib.update(width='60', height='60')
        elif name == 'inbox_msg':
            widget(root, 'msg/share/thumb').set('height', '104')
            for key in ('avatar/talker', 'avatar/mine'):
                widget(root, key).attrib.update(width='30', height='30')
        elif name == 'live_player_activity':
            root.set('footerHidden', 'true')
            widget(root, 'live_detail_left_box').set('margin', '3')
            widget(root, 'live_title_box').attrib.update(height='30', marginTop='3')
            widget(root, 'live/title').attrib.update(fontSize='19', marginLeft='2')
            widget(root, 'live_detail_info_box').set('height', '52')
            widget(root, 'live_author').attrib.update(height='32', marginLeft='2')
            widget(root, 'live_danmaku_sidebar').attrib.update(width='auto', marginLeft='6', marginRight='0')
        elif name == 'hint_activity':
            widget(root, 'hint/gallery').attrib.update(paddingLeft='12', paddingRight='12')
        elif name == 'pgc_index_activity':
            root.set('footerHidden', 'true')
            widget(root, 'pgc/title').attrib.update(marginTop='2', marginLeft='8', marginRight='8')
            root[0][1].set('marginBottom', '4')
        elif name == 'dynamic_detail':
            root.set('padding', '8')
            root[-1].attrib.update(positionTop='6', positionLeft='12', fontSize='18')
            widget(root, 'dynamic/detail/grid').set('marginTop', '26')
        elif name.startswith('player_comment_action'):
            # Keep all action buttons reachable even when the optional delete/gallery controls appear.
            root.attrib.update(axis='column', padding='8', justifyContent='flexStart')
            actions = widget(root, 'comment/action/box')
            actions.attrib = dict(id=actions.get('id'), width='100%', height='38', justifyContent='center',
                                  alignItems='center')
            buttons = list(actions.iter('SVGImage'))
            row = ET.Element('brls-Box', alignItems='center')
            for button in buttons:
                button.attrib.update(width='26', height='26', margin='4')
                row.append(button)
            actions[:] = [row]
            panel = widget(root, 'box/background')
            panel.attrib.update(width='100%', grow='1', height='auto', axis='column', padding='6')
            panel[0].attrib.update(positionType='relative', positionTop='0', positionLeft='0', fontSize='18')
            scroll = panel.find('brls-ScrollingFrame')
            scroll.attrib.update(width='100%', marginLeft='0', marginTop='4')
            scroll[0].attrib.update(width='100%', paddingLeft='4', paddingRight='4')
            widget(root, 'comment/action/comment').set('width', '100%')
            if root[0].get('id') == 'player/cancel':
                root[0].attrib.update(positionType='absolute', width='100%', height='100%')
        elif name in ('player_single_comment', 'player_single_comment_dynamic', 'player_setting', 'player_danmaku_setting'):
            root.set('padding', '8')
            panel = next(child for child in root if child.get('backgroundColor') == '@theme/brls/background')
            panel.set('width', '96%')
            for grid in panel.iter('RecyclingGrid'):
                grid.attrib.update(width='100%', marginTop='36')
            for label in panel.findall('brls-Label'):
                label.attrib.update(positionTop='8', positionLeft='10', fontSize='18')
        elif name in ('player_coin', 'player_coin_psv', 'player_collection'):
            root.set('padding', '8')
            root[1].attrib.update(fontSize='20', marginTop='2', marginBottom='6')
            if name == 'player_collection':
                root[-1].set('height', '138')
                root[-1][0].set('itemHeight', '42')
            else:
                row = root[3]
                row.attrib.update(height='90', marginTop='4', marginBottom='4')
                for image in row:
                    image.attrib.update(height='90', width='60')
                widget(root, 'coin/hint').set('marginBottom', '2')
                widget(root, 'coin/like').attrib.update(positionType='relative', positionBottom='0', positionRight='0',
                                                       justifyContent='center', height='26')
        elif name == 'player_evaluate':
            root[0].attrib.update(height='152', marginLeft='10', marginTop='8')
            widget(root, 'img/empty').attrib.update(height='110', positionTop='12')
        elif name in ('setting_network', 'settings_hot_keys', 'settings_tutorial_error', 'settings_tutorial_font'):
            root.set('margin', '8')
            for item in root:
                if 'marginLeft' in item.attrib:
                    item.set('marginLeft', '6')
                if 'marginBottom' in item.attrib:
                    item.set('marginBottom', '6')
        elif name == 'season_evaluate':
            content = widget(root, 'evaluate/content')
            scroll = ET.Element('brls-ScrollingFrame', height='128', focusable='true')
            root.remove(content)
            root.insert(0, scroll)
            scroll.append(content)
            content.attrib.update(margin='8', marginTop='6')
        elif name == 'latest_update':
            root.attrib.update(height='174', marginTop='4', marginBottom='4')
            root.find('brls-ScrollingFrame')[0].attrib.update(minHeight='0', paddingLeft='12', paddingRight='12')
            widget(root, 'latest/author').set('height', '38')
            widget(root, 'latest/header').attrib.update(fontSize='21', marginTop='3', marginBottom='6')
        elif name == 'share_dialog':
            root.set('padding', '8')
            root[1].set('margin', '4')
            widget(root, 'share/qrcode').attrib.update(width='136', height='136', margin='4')
            widget(root, 'share/box').attrib.update(marginLeft='6', paddingLeft='6', paddingRight='6')
        elif name == 'share_box':
            root.set('padding', '4')
            widget(root, 'share/image').attrib.update(width='32', height='32', marginBottom='4')
            widget(root, 'share/title').set('fontSize', '15')
        elif name in ('grid_dropdown', 'home_live_area'):
            root.set('width', '100%')
            panel = widget(root, 'grid_dropdown/content')
            panel.set('padding', '8')
            widget(root, 'grid_dropdown/header').attrib.update(height='30', paddingTop='0', paddingBottom='0',
                                                              paddingLeft='6', paddingRight='6', marginLeft='0', marginRight='0')
            widget(root, 'grid_dropdown/title_label').attrib.update(fontSize='18', marginTop='0')
            if name == 'home_live_area':
                widget(root, 'area/mainList').attrib.update(width='120', itemHeight='34', marginTop='4')
                widget(root, 'area/subList').attrib.update(spanCount='3', itemHeight='54')
            else:
                panel.set('height', '195')
                widget(root, 'grid_dropdown/recycler').set('itemHeight', '38')

        if ET.tostring(root) != before:
            save(root, target, path)


def fit_text(source, target, *, lcd):
    """Size captions in the existing render target. Each card keeps its original line count."""
    cards = ('video_card', 'video_card_history', 'video_card_live', 'video_card_pgc')
    for original in sorted(source.glob('*/*.xml')):
        path = original.relative_to(source)
        root = layout(target if (target / path).exists() else source, path)
        before = ET.tostring(root)
        name = path.stem
        if name in cards:
            cover, caption = root
            title, byline = caption[:2]
            title_size, line_height, author_size = (20.4, 24, 17) if lcd else (24, 29, 19.5)
            rows = int(title.get('maxRows'))
            title.attrib.update(fontSize=str(title_size), lineHeight=f'{line_height / title_size:.3f}',
                                height=str(rows * line_height))
            byline.attrib.update(height='22' if lcd else '25', grow='0', shrink='0', marginTop='1')
            widget(byline, 'video/card/label/username').attrib.update(fontSize=str(author_size), grow='1', shrink='1')
            cover.attrib.update(grow='1', shrink='1')
            caption.set('shrink', '0')
            if not lcd:
                caption.set('margin', '6')
        elif name == 'dynamic_tab' and not lcd:
            fit_dynamic_feed(root, lcd=False)
        elif name == 'player_activity' and not lcd:
            _, byline, _ = widget(root, 'video_detail_info_box')
            byline.attrib.update(height='78', alignItems='center')
            author, actions = byline
            author.attrib.update(width='auto', grow='1', shrink='1')
            actions.attrib.update(width='184', height='58', marginLeft='16', marginRight='6',
                                  marginBottom='0', shrink='0')
            for action in actions:
                action.attrib.update(width='44', height='54', padding='0', justifyContent='center')
                action[0].attrib.update(width='26', height='26', positionTop='0')
                action[1].attrib.update(positionType='relative', positionBottom='0', width='100%',
                                        fontSize='14', marginTop='5', singleLine='true')
        elif name == 'mine_tab':
            widget(root, 'mine/label/sign').set('fontSize', '17' if lcd else '19.5')
            for tab in root.iter('Tab'):
                tab.set('fontSize', '19' if lcd else '20')
        elif name == 'inbox_view':
            for tab in root.iter('Tab'):
                tab.set('fontSize', '19' if lcd else '24')
        elif name == 'inbox_user':
            widget(root, 'inbox/lastTime').set('fontSize', '17' if lcd else '19.5')
        elif name == 'hots_card':
            widget(root, 'hots/content').set('fontSize', '20.4' if lcd else '22')
        for node in root.iter():
            if node.get('textColor') == '@theme/font/grey':
                node.set('textColor', '@theme/font/secondary')
        if ET.tostring(root) != before:
            save(root, target, path)
